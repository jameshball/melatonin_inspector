#include <juce_core/juce_core.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if JUCE_WINDOWS
    #include <windows.h>
#endif

namespace
{
    juce::File sessionsDirectory()
    {
        const char* temp = std::getenv (
#if JUCE_WINDOWS
            "TEMP"
#else
            "TMPDIR"
#endif
        );

        return juce::File (temp != nullptr ? juce::String::fromUTF8 (temp) : juce::String ("/tmp"))
            .getChildFile ("melatonin_inspector")
            .getChildFile ("sessions");
    }

    bool canConnect (juce::DynamicObject& session)
    {
        juce::StreamingSocket socket;
        const auto host = session.getProperty ("host").toString();
        const auto port = (int) session.getProperty ("port");

        return host.isNotEmpty() && port > 0 && socket.connect (host, port, 250);
    }

    juce::Array<juce::var> loadSessions()
    {
        juce::Array<juce::var> sessions;
        for (const auto& entry : juce::RangedDirectoryIterator (sessionsDirectory(), false, "*.json", juce::File::findFiles))
        {
            auto file = entry.getFile();
            auto parsed = juce::JSON::parse (file.loadFileAsString());

            if (auto* session = parsed.getDynamicObject())
            {
                session->setProperty ("file", file.getFullPathName());
                session->setProperty ("modifiedAtMs", (double) file.getLastModificationTime().toMilliseconds());

                if (canConnect (*session))
                    sessions.add (parsed);
            }
        }

        return sessions;
    }

    juce::var findSession (const juce::String& requestedName)
    {
        auto sessions = loadSessions();

        if (requestedName.isEmpty() && sessions.size() == 1)
            return sessions.getFirst();

        if (requestedName.isEmpty())
            return {};

        juce::var bestMatch;
        double bestModifiedAt = -1.0;

        for (auto& session : sessions)
        {
            auto* object = session.getDynamicObject();

            if (object == nullptr)
                continue;

            if (object->getProperty ("session").toString() == requestedName
                || object->getProperty ("pid").toString() == requestedName
                || object->getProperty ("file").toString().contains (requestedName))
            {
                const auto modifiedAt = (double) object->getProperty ("modifiedAtMs");

                if (modifiedAt > bestModifiedAt)
                {
                    bestModifiedAt = modifiedAt;
                    bestMatch = session;
                }
            }
        }

        return bestMatch;
    }

    juce::String readLine (juce::StreamingSocket& socket, int timeoutMs)
    {
        std::string bytes;
        char buffer[1024] {};
        const auto deadline = juce::Time::currentTimeMillis() + juce::jmax (1, timeoutMs);

        while (juce::Time::currentTimeMillis() < deadline)
        {
            const auto remaining = (int) (deadline - juce::Time::currentTimeMillis());
            const auto ready = socket.waitUntilReady (true, juce::jlimit (1, 250, remaining));

            if (ready < 0)
                break;

            if (ready == 0)
                continue;

            const auto bytesRead = socket.read (buffer, (int) sizeof (buffer), false);

            if (bytesRead <= 0)
                break;

            bytes.append (buffer, (size_t) bytesRead);

            if (bytes.find ('\n') != std::string::npos)
                break;
        }

        if (auto newline = bytes.find ('\n'); newline != std::string::npos)
            bytes.resize (newline);

        return juce::String::fromUTF8 (bytes.data(), (int) bytes.size()).trim();
    }

    int responseTimeoutMs (const juce::String& method, const juce::var& params)
    {
        int endpointTimeoutMs = method == "wait" ? 250 : (method.startsWith ("wait_for") ? 5000 : 5000);

        if (auto* object = params.getDynamicObject())
        {
            auto timeout = object->getProperty (method == "wait" ? "ms" : "timeoutMs");

            if (!timeout.isVoid())
                endpointTimeoutMs = (int) timeout;
        }

        return juce::jlimit (5000, 35000, endpointTimeoutMs + 2000);
    }

    juce::String endpointErrorMessage (juce::DynamicObject& responseObject)
    {
        auto* error = responseObject.getProperty ("error").getDynamicObject();
        auto message = error != nullptr ? error->getProperty ("message").toString() : "Unknown automation error";

        if (error != nullptr)
        {
            auto code = error->getProperty ("code").toString();

            if (code.isNotEmpty())
                message = code + ": " + message;

            auto matchCount = error->getProperty ("matchCount");

            if (!matchCount.isVoid())
                message << "\nmatchCount=" << matchCount.toString();

            auto matches = error->getProperty ("matches");

            if (!matches.isVoid())
                message << "\nmatches=" << juce::JSON::toString (matches, true);

            auto suggested = error->getProperty ("suggestedNextCommand").toString();

            if (suggested.isNotEmpty())
                message << "\nsuggestedNextCommand=" << suggested;
        }

        return message;
    }

    juce::var requestEnvelope (juce::DynamicObject& session, const juce::String& method, juce::var params)
    {
        juce::StreamingSocket socket;
        const auto host = session.getProperty ("host").toString();
        const auto port = (int) session.getProperty ("port");

        if (!socket.connect (host, port, 3000))
            throw std::runtime_error ("Could not connect to " + host.toStdString() + ":" + std::to_string (port));

        auto* requestObject = new juce::DynamicObject();
        requestObject->setProperty ("id", "1");
        requestObject->setProperty ("token", session.getProperty ("token"));
        requestObject->setProperty ("method", method);
        requestObject->setProperty ("params", params);

        auto payload = juce::JSON::toString (juce::var (requestObject), true) + "\n";
        socket.write (payload.toRawUTF8(), (int) payload.getNumBytesAsUTF8());

        auto response = juce::JSON::parse (readLine (socket, responseTimeoutMs (method, params)));
        auto* responseObject = response.getDynamicObject();

        if (responseObject == nullptr)
            throw std::runtime_error ("Timed out waiting for automation endpoint response");

        return response;
    }

    juce::var request (juce::DynamicObject& session, const juce::String& method, juce::var params)
    {
        auto response = requestEnvelope (session, method, params);
        auto* responseObject = response.getDynamicObject();

        if (responseObject == nullptr)
            throw std::runtime_error ("Invalid response from automation endpoint");

        if (! (bool) responseObject->getProperty ("ok"))
            throw std::runtime_error (endpointErrorMessage (*responseObject).toStdString());

        return responseObject->getProperty ("result");
    }

    juce::String optionValue (juce::StringArray& args, const juce::String& option, const juce::String& fallback = {})
    {
        auto index = args.indexOf (option);

        if (index < 0 || index + 1 >= args.size())
            return fallback;

        auto value = args[index + 1];
        args.remove (index + 1);
        args.remove (index);
        return value;
    }

    bool hasFlag (juce::StringArray& args, const juce::String& option)
    {
        auto index = args.indexOf (option);

        if (index < 0)
            return false;

        args.remove (index);
        return true;
    }

    juce::File supportDirectoryForHome (const juce::File& home)
    {
#if JUCE_MAC
        return home.getChildFile ("Library").getChildFile ("Application Support");
#elif JUCE_WINDOWS
        return home.getChildFile ("AppData").getChildFile ("Roaming");
#else
        return home.getChildFile (".config");
#endif
    }

#if JUCE_WINDOWS
    juce::File windowsRoamingDataDirectoryForHome (const juce::File& home)
    {
        return home.getChildFile ("AppData").getChildFile ("Roaming");
    }

    juce::File windowsLocalDataDirectoryForHome (const juce::File& home)
    {
        return home.getChildFile ("AppData").getChildFile ("Local");
    }
#elif ! JUCE_MAC
    juce::File linuxConfigDirectoryForHome (const juce::File& home)
    {
        return home.getChildFile (".config");
    }

    juce::File linuxDataDirectoryForHome (const juce::File& home)
    {
        return home.getChildFile (".local").getChildFile ("share");
    }

    juce::File linuxCacheDirectoryForHome (const juce::File& home)
    {
        return home.getChildFile (".cache");
    }
#endif

    juce::StringArray automationEnvironmentForLaunch (const juce::File& home,
                                                      const juce::File& artifactDir,
                                                      const juce::String& sessionName)
    {
        juce::StringArray environment;

        auto add = [&] (const juce::String& name, const juce::String& value)
        {
            environment.add (name + "=" + value);
        };

        add ("MELATONIN_INSPECTOR_AUTOMATION", "1");
        add ("MELATONIN_INSPECTOR_SESSION", sessionName);
        add ("MELATONIN_INSPECTOR_ARTIFACT_ROOT", artifactDir.getFullPathName());

#if JUCE_MAC
        add ("HOME", home.getFullPathName());
        add ("CFFIXED_USER_HOME", home.getFullPathName());
#elif JUCE_WINDOWS
        auto roaming = windowsRoamingDataDirectoryForHome (home);
        auto local = windowsLocalDataDirectoryForHome (home);
        roaming.createDirectory();
        local.createDirectory();

        add ("USERPROFILE", home.getFullPathName());
        add ("HOME", home.getFullPathName());
        add ("APPDATA", roaming.getFullPathName());
        add ("LOCALAPPDATA", local.getFullPathName());

        auto homePath = home.getFullPathName();
        if (homePath.length() >= 2 && homePath[1] == ':')
        {
            add ("HOMEDRIVE", homePath.substring (0, 2));
            add ("HOMEPATH", homePath.substring (2));
        }
#else
        auto config = linuxConfigDirectoryForHome (home);
        auto data = linuxDataDirectoryForHome (home);
        auto cache = linuxCacheDirectoryForHome (home);
        config.createDirectory();
        data.createDirectory();
        cache.createDirectory();

        add ("HOME", home.getFullPathName());
        add ("XDG_CONFIG_HOME", config.getFullPathName());
        add ("XDG_DATA_HOME", data.getFullPathName());
        add ("XDG_CACHE_HOME", cache.getFullPathName());
#endif

        return environment;
    }

    void addEnvironmentToCommand (juce::StringArray& command, const juce::StringArray& environment)
    {
        for (const auto& assignment : environment)
            command.add (assignment);
    }

#if JUCE_WINDOWS
    void appendBackslashes (juce::String& text, int count)
    {
        while (count-- > 0)
            text += "\\";
    }

    juce::String quoteWindowsCommandLineArgument (const juce::String& argument)
    {
        if (argument.isNotEmpty() && ! argument.containsAnyOf (" \t\n\v\""))
            return argument;

        juce::String quoted = "\"";
        int backslashes = 0;

        for (auto character = argument.getCharPointer(); ! character.isEmpty();)
        {
            auto c = character.getAndAdvance();

            if (c == '\\')
            {
                ++backslashes;
                continue;
            }

            if (c == '"')
            {
                appendBackslashes (quoted, backslashes * 2 + 1);
                quoted += "\"";
                backslashes = 0;
                continue;
            }

            appendBackslashes (quoted, backslashes);
            backslashes = 0;
            quoted += juce::String::charToString (c);
        }

        appendBackslashes (quoted, backslashes * 2);
        quoted += "\"";
        return quoted;
    }

    juce::String windowsCommandLineForApp (const juce::File& app, const juce::StringArray& appArgs)
    {
        juce::StringArray arguments;
        arguments.add (app.getFullPathName());
        arguments.addArray (appArgs);

        juce::StringArray quoted;
        for (const auto& argument : arguments)
            quoted.add (quoteWindowsCommandLineArgument (argument));

        return quoted.joinIntoString (" ");
    }

    juce::String windowsLastErrorMessage (const juce::String& context)
    {
        const auto error = GetLastError();
        LPWSTR message = nullptr;

        FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr,
                        error,
                        MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                        reinterpret_cast<LPWSTR> (&message),
                        0,
                        nullptr);

        juce::String result = context + " failed";
        if (message != nullptr)
        {
            result += ": ";
            result += juce::String (message).trim();
            LocalFree (message);
        }

        result += " (";
        result += juce::String ((int) error);
        result += ")";
        return result;
    }

    std::vector<wchar_t> wideNullTerminatedString (const juce::String& text)
    {
        std::vector<wchar_t> result;

        for (auto character = text.toWideCharPointer(); ! character.isEmpty();)
            result.push_back ((wchar_t) character.getAndAdvance());

        result.push_back (L'\0');
        return result;
    }

    juce::String environmentAssignmentName (const juce::String& assignment)
    {
        return assignment.upToFirstOccurrenceOf ("=", false, false);
    }

    std::vector<wchar_t> windowsEnvironmentBlock (const juce::StringArray& overrides)
    {
        juce::StringArray entries;

        if (auto* environment = GetEnvironmentStringsW())
        {
            for (auto* entry = environment; *entry != L'\0'; entry += wcslen (entry) + 1)
                entries.add (juce::String (entry));

            FreeEnvironmentStringsW (environment);
        }

        for (const auto& override : overrides)
        {
            auto name = environmentAssignmentName (override);

            for (int i = entries.size(); --i >= 0;)
                if (environmentAssignmentName (entries[i]).equalsIgnoreCase (name))
                    entries.remove (i);

            entries.add (override);
        }

        entries.sort (true);

        std::vector<wchar_t> block;
        for (const auto& entry : entries)
        {
            auto wide = wideNullTerminatedString (entry);
            block.insert (block.end(), wide.begin(), wide.end());
        }

        block.push_back (L'\0');
        return block;
    }

    HANDLE createWindowsLogFile (const juce::File& file)
    {
        SECURITY_ATTRIBUTES attributes {};
        attributes.nLength = sizeof (attributes);
        attributes.bInheritHandle = TRUE;

        auto path = wideNullTerminatedString (file.getFullPathName());
        return CreateFileW (path.data(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &attributes,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    }

    void launchWindowsApp (const juce::File& app,
                           const juce::StringArray& appArgs,
                           const juce::StringArray& environment,
                           const juce::File& stdoutFile,
                           const juce::File& stderrFile)
    {
        auto stdoutHandle = createWindowsLogFile (stdoutFile);
        if (stdoutHandle == INVALID_HANDLE_VALUE)
            throw std::runtime_error (windowsLastErrorMessage ("CreateFile stdout").toStdString());

        auto stderrHandle = createWindowsLogFile (stderrFile);
        if (stderrHandle == INVALID_HANDLE_VALUE)
        {
            CloseHandle (stdoutHandle);
            throw std::runtime_error (windowsLastErrorMessage ("CreateFile stderr").toStdString());
        }

        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof (startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = GetStdHandle (STD_INPUT_HANDLE);
        startupInfo.hStdOutput = stdoutHandle;
        startupInfo.hStdError = stderrHandle;

        PROCESS_INFORMATION processInfo {};
        auto commandLine = wideNullTerminatedString (windowsCommandLineForApp (app, appArgs));
        auto environmentBlock = windowsEnvironmentBlock (environment);
        auto workingDirectory = wideNullTerminatedString (app.getParentDirectory().getFullPathName());

        auto launched = CreateProcessW (nullptr,
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_UNICODE_ENVIRONMENT,
                                        environmentBlock.data(),
                                        workingDirectory.data(),
                                        &startupInfo,
                                        &processInfo);

        CloseHandle (stdoutHandle);
        CloseHandle (stderrHandle);

        if (! launched)
            throw std::runtime_error (windowsLastErrorMessage ("CreateProcess").toStdString());

        CloseHandle (processInfo.hThread);
        CloseHandle (processInfo.hProcess);
    }
#endif

    juce::String appNameFromPath (const juce::String& path)
    {
        if (path.isEmpty())
            return {};

        auto file = juce::File::createFileWithoutCheckingPath (path);
        auto current = file;

        while (current != juce::File())
        {
            if (current.getFileExtension().equalsIgnoreCase (".app"))
                return current.getFileNameWithoutExtension();

            auto parent = current.getParentDirectory();

            if (parent == current)
                break;

            current = parent;
        }

        return file.getFileNameWithoutExtension();
    }

    juce::File macBundleForAppPath (const juce::File& app)
    {
        auto current = app;

        while (current != juce::File())
        {
            if (current.getFileExtension().equalsIgnoreCase (".app") && current.isDirectory())
                return current;

            auto parent = current.getParentDirectory();

            if (parent == current)
                break;

            current = parent;
        }

        return {};
    }

    juce::XmlElement* findPropertyValue (juce::XmlElement& root, const juce::String& name)
    {
        for (auto* child : root.getChildIterator())
            if (child != nullptr && child->hasTagName ("VALUE") && child->getStringAttribute ("name") == name)
                return child;

        return nullptr;
    }

    void removePropertyValues (juce::XmlElement& root, const juce::String& name)
    {
        for (int i = root.getNumChildElements(); --i >= 0;)
        {
            auto* child = root.getChildElement (i);

            if (child != nullptr && child->hasTagName ("VALUE") && child->getStringAttribute ("name") == name)
                root.removeChildElement (child, true);
        }
    }

    juce::XmlElement& getOrCreatePropertyValue (juce::XmlElement& root, const juce::String& name)
    {
        if (auto* existing = findPropertyValue (root, name))
            return *existing;

        auto* value = root.createNewChildElement ("VALUE");
        value->setAttribute ("name", name);
        return *value;
    }

    void disableJuceAudioRestore (juce::XmlElement& root)
    {
        auto& value = getOrCreatePropertyValue (root, "audioSetup");
        value.deleteAllChildElements();

        auto* setup = value.createNewChildElement ("DEVICESETUP");
        setup->setAttribute ("deviceType", "");
        setup->setAttribute ("audioOutputDeviceName", "");
        setup->setAttribute ("audioInputDeviceName", "");
        setup->setAttribute ("audioDeviceInChans", "0");
        setup->setAttribute ("audioDeviceOutChans", "0");
    }

    void copySettingFileIfPresent (const juce::File& sourceSupport,
                                   const juce::File& targetSupport,
                                   const juce::String& fileName,
                                   juce::StringArray& copied)
    {
        if (fileName.isEmpty())
            return;

        auto source = sourceSupport.getChildFile (fileName);

        if (! source.existsAsFile())
            return;

        auto target = targetSupport.getChildFile (fileName);
        target.deleteFile();

        if (! source.copyFileTo (target))
            throw std::runtime_error (("Could not copy settings file to " + target.getFullPathName()).toStdString());

        copied.addIfNotAlreadyThere (target.getFullPathName());
    }

    juce::var prepareJuceProfile (const juce::File& home,
                                  const juce::File& sourceHome,
                                  const juce::String& appName,
                                  const juce::StringArray& extraSettings,
                                  bool clearFilterState,
                                  bool disableAudioRestore)
    {
        if (appName.isEmpty())
            throw std::runtime_error ("prepare-juce-profile requires --app-name or --app");

        auto sourceSupport = supportDirectoryForHome (sourceHome);
        auto targetSupport = supportDirectoryForHome (home);
        juce::StringArray copied;

        if (! targetSupport.createDirectory())
            throw std::runtime_error (("Could not create profile support directory " + targetSupport.getFullPathName()).toStdString());

        if (sourceSupport.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (sourceSupport, false, "*.settings", juce::File::findFiles))
            {
                auto file = entry.getFile();
                auto fileName = file.getFileName();

                if (! fileName.matchesWildcard (appName + "*.settings", false))
                    continue;

                auto target = targetSupport.getChildFile (fileName);
                target.deleteFile();

                if (! file.copyFileTo (target))
                    throw std::runtime_error (("Could not copy settings file to " + target.getFullPathName()).toStdString());

                copied.addIfNotAlreadyThere (target.getFullPathName());
            }

            for (const auto& setting : extraSettings)
                copySettingFileIfPresent (sourceSupport, targetSupport, setting, copied);
        }

        auto mainSettings = targetSupport.getChildFile (appName + ".settings");
        std::unique_ptr<juce::XmlElement> root;

        if (mainSettings.existsAsFile())
            root = juce::parseXML (mainSettings);

        if (root == nullptr || ! root->hasTagName ("PROPERTIES"))
            root = std::make_unique<juce::XmlElement> ("PROPERTIES");

        if (clearFilterState)
            removePropertyValues (*root, "filterState");

        if (disableAudioRestore)
            disableJuceAudioRestore (*root);

        if (! mainSettings.replaceWithText (root->toString()))
            throw std::runtime_error (("Could not write settings file " + mainSettings.getFullPathName()).toStdString());

        copied.addIfNotAlreadyThere (mainSettings.getFullPathName());

        juce::Array<juce::var> copiedFiles;
        for (const auto& file : copied)
            copiedFiles.add (file);

        auto* result = new juce::DynamicObject();
        result->setProperty ("home", home.getFullPathName());
        result->setProperty ("supportDirectory", targetSupport.getFullPathName());
        result->setProperty ("appName", appName);
        result->setProperty ("settingsFile", mainSettings.getFullPathName());
        result->setProperty ("copied", juce::var (copiedFiles));
        result->setProperty ("clearedFilterState", clearFilterState);
        result->setProperty ("disabledAudioRestore", disableAudioRestore);
        return juce::var (result);
    }

    juce::var waitForSession (const juce::String& sessionName, int timeoutMs)
    {
        const auto deadline = juce::Time::currentTimeMillis() + juce::jmax (1, timeoutMs);

        while (juce::Time::currentTimeMillis() < deadline)
        {
            auto session = findSession (sessionName);

            if (session.getDynamicObject() != nullptr)
                return session;

            juce::Thread::sleep (250);
        }

        return {};
    }

    void addRepeatedOptionValues (juce::StringArray& values, juce::StringArray& args, const juce::String& option)
    {
        for (;;)
        {
            auto value = optionValue (args, option);

            if (value.isEmpty())
                break;

            values.addIfNotAlreadyThere (value);
        }
    }

    juce::StringArray remainingArgsAfterDoubleDash (juce::StringArray& args)
    {
        juce::StringArray result;
        auto separator = args.indexOf ("--");

        if (separator < 0)
            return result;

        for (int i = separator + 1; i < args.size(); ++i)
            result.add (args[i]);

        while (args.size() > separator)
            args.remove (separator);

        return result;
    }

    juce::var launchAutomationApp (juce::StringArray& args)
    {
        auto appPath = optionValue (args, "--app");
        auto appName = optionValue (args, "--app-name", appNameFromPath (appPath));
        auto sessionName = optionValue (args, "--session", appName);
        auto artifactPath = optionValue (args, "--artifact-dir",
                                         juce::File::getSpecialLocation (juce::File::tempDirectory)
                                             .getChildFile (sessionName + "-melatonin-automation")
                                             .getFullPathName());
        auto homePath = optionValue (args, "--home", juce::File::createFileWithoutCheckingPath (artifactPath).getChildFile ("home").getFullPathName());
        auto sourceHomePath = optionValue (args, "--source-home", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName());
        auto stdoutPath = optionValue (args, "--stdout", juce::File::createFileWithoutCheckingPath (artifactPath).getChildFile ("app.stdout.log").getFullPathName());
        auto stderrPath = optionValue (args, "--stderr", juce::File::createFileWithoutCheckingPath (artifactPath).getChildFile ("app.stderr.log").getFullPathName());
        auto timeoutMs = optionValue (args, "--timeout-ms", "30000").getIntValue();
        auto noWait = hasFlag (args, "--no-wait");
        auto noProfile = hasFlag (args, "--no-profile");
        auto keepFilterState = hasFlag (args, "--keep-filter-state");
        auto keepAudioState = hasFlag (args, "--keep-audio-state");
        juce::StringArray extraSettings;
        addRepeatedOptionValues (extraSettings, args, "--copy-setting");
        auto appArgs = remainingArgsAfterDoubleDash (args);

        if (appPath.isEmpty())
            throw std::runtime_error ("launch requires --app");

        if (appName.isEmpty())
            throw std::runtime_error ("launch requires --app-name when it cannot be inferred from --app");

        if (sessionName.isEmpty())
            throw std::runtime_error ("launch requires --session when it cannot be inferred from --app-name");

        auto app = juce::File::createFileWithoutCheckingPath (appPath);
        auto artifactDir = juce::File::createFileWithoutCheckingPath (artifactPath);
        auto home = juce::File::createFileWithoutCheckingPath (homePath);
        auto sourceHome = juce::File::createFileWithoutCheckingPath (sourceHomePath);
        auto stdoutFile = juce::File::createFileWithoutCheckingPath (stdoutPath);
        auto stderrFile = juce::File::createFileWithoutCheckingPath (stderrPath);

        if (! artifactDir.createDirectory())
            throw std::runtime_error (("Could not create artifact directory " + artifactDir.getFullPathName()).toStdString());

        stdoutFile.getParentDirectory().createDirectory();
        stderrFile.getParentDirectory().createDirectory();

        juce::var profile;
        if (! noProfile)
            profile = prepareJuceProfile (home, sourceHome, appName, extraSettings, ! keepFilterState, ! keepAudioState);

        auto launchEnvironment = automationEnvironmentForLaunch (home, artifactDir, sessionName);
        juce::StringArray command;

#if JUCE_MAC
        auto bundle = macBundleForAppPath (app);
        if (bundle.isDirectory())
        {
            command.add ("/usr/bin/open");
            command.add ("-n");
            command.add ("-F");
            command.add ("--stdout");
            command.add (stdoutFile.getFullPathName());
            command.add ("--stderr");
            command.add (stderrFile.getFullPathName());

            for (const auto& assignment : launchEnvironment)
            {
                command.add ("--env");
                command.add (assignment);
            }

            command.add (bundle.getFullPathName());

            if (! appArgs.isEmpty())
            {
                command.add ("--args");
                command.addArray (appArgs);
            }
        }
        else
#endif
        {
#if JUCE_WINDOWS
            command.add (app.getFullPathName());
            command.addArray (appArgs);
#else
            command.add ("/usr/bin/env");
            addEnvironmentToCommand (command, launchEnvironment);
            command.add (app.getFullPathName());
            command.addArray (appArgs);
#endif
        }

#if JUCE_WINDOWS
        launchWindowsApp (app, appArgs, launchEnvironment, stdoutFile, stderrFile);
#else
        juce::ChildProcess process;
        if (! process.start (command, 0))
            throw std::runtime_error ("Could not launch app");

        process.waitForProcessToFinish (2000);
#endif
        auto session = noWait ? juce::var() : waitForSession (sessionName, timeoutMs);

        if (! noWait && session.getDynamicObject() == nullptr)
            throw std::runtime_error (("Timed out waiting for melatonin_inspector session '" + sessionName
                                       + "'. stdout=" + stdoutFile.getFullPathName()
                                       + " stderr=" + stderrFile.getFullPathName()).toStdString());

        juce::Array<juce::var> commandJson;
        for (const auto& arg : command)
            commandJson.add (arg);

        auto* result = new juce::DynamicObject();
        result->setProperty ("app", app.getFullPathName());
        result->setProperty ("appName", appName);
        result->setProperty ("session", sessionName);
        result->setProperty ("artifactDir", artifactDir.getFullPathName());
        result->setProperty ("home", home.getFullPathName());
        result->setProperty ("stdout", stdoutFile.getFullPathName());
        result->setProperty ("stderr", stderrFile.getFullPathName());
        result->setProperty ("command", juce::var (commandJson));

        juce::Array<juce::var> environmentJson;
        for (const auto& assignment : launchEnvironment)
            environmentJson.add (assignment);

        result->setProperty ("environment", juce::var (environmentJson));

        if (! profile.isVoid())
            result->setProperty ("profile", profile);

        if (session.getDynamicObject() != nullptr)
            result->setProperty ("matchedSession", session);

        return juce::var (result);
    }

    juce::var parseValue (const juce::String& text)
    {
        if (text == "true")
            return true;

        if (text == "false")
            return false;

        if (text.containsOnly ("-0123456789"))
            return text.getIntValue();

        if (text.containsOnly ("-0123456789."))
            return text.getDoubleValue();

        return text;
    }

    juce::var parseLocatorOptions (juce::StringArray& args)
    {
        auto locator = std::make_unique<juce::DynamicObject>();
        bool hasLocator = false;

        auto addString = [&] (const juce::String& option, const juce::String& property) {
            auto value = optionValue (args, option);

            if (value.isNotEmpty())
            {
                locator->setProperty (property, value);
                hasLocator = true;
            }
        };

        addString ("--role", "role");
        addString ("--name", "name");
        addString ("--text", "text");
        addString ("--component-id", "componentId");
        addString ("--component-name", "componentName");
        addString ("--test-id", "testId");
        addString ("--class", "class");
        addString ("--value", "value");
        addString ("--has-text", "hasText");

        auto nth = optionValue (args, "--nth");

        if (nth.isNotEmpty())
        {
            locator->setProperty ("nth", nth.getIntValue());
            hasLocator = true;
        }

        if (hasFlag (args, "--exact"))
        {
            locator->setProperty ("exact", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--visible"))
        {
            locator->setProperty ("visible", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--hidden"))
        {
            locator->setProperty ("visible", false);
            hasLocator = true;
        }

        if (hasFlag (args, "--enabled"))
        {
            locator->setProperty ("enabled", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--disabled"))
        {
            locator->setProperty ("enabled", false);
            hasLocator = true;
        }

        if (hasFlag (args, "--focused"))
        {
            locator->setProperty ("focused", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--selected"))
        {
            locator->setProperty ("selected", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--not-selected"))
        {
            locator->setProperty ("selected", false);
            hasLocator = true;
        }

        if (hasFlag (args, "--accessible"))
        {
            locator->setProperty ("accessible", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--inaccessible"))
        {
            locator->setProperty ("accessible", false);
            hasLocator = true;
        }

        return hasLocator ? juce::var (locator.release()) : juce::var();
    }

    juce::var parseTargetLocatorOptions (juce::StringArray& args)
    {
        auto locator = std::make_unique<juce::DynamicObject>();
        bool hasLocator = false;

        auto addString = [&] (const juce::String& flag, const juce::String& property)
        {
            auto value = optionValue (args, flag);

            if (value.isNotEmpty())
            {
                locator->setProperty (property, value);
                hasLocator = true;
            }
        };

        addString ("--target-role", "role");
        addString ("--target-name", "name");
        addString ("--target-text", "text");
        addString ("--target-component-id", "componentId");
        addString ("--target-component-name", "componentName");
        addString ("--target-test-id", "testId");
        addString ("--target-class", "class");
        addString ("--target-value", "value");

        auto nth = optionValue (args, "--target-nth");

        if (nth.isNotEmpty())
        {
            locator->setProperty ("nth", nth.getIntValue());
            hasLocator = true;
        }

        if (hasFlag (args, "--target-exact"))
        {
            locator->setProperty ("exact", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--target-accessible"))
        {
            locator->setProperty ("accessible", true);
            hasLocator = true;
        }

        if (hasFlag (args, "--target-inaccessible"))
        {
            locator->setProperty ("accessible", false);
            hasLocator = true;
        }

        return hasLocator ? juce::var (locator.release()) : juce::var();
    }

    void addLocatorIfPresent (juce::DynamicObject& params, const juce::var& locator)
    {
        if (!locator.isVoid())
            params.setProperty ("locator", locator);
    }

    void addSnapshotOptions (juce::StringArray& args, juce::DynamicObject& params)
    {
        auto mode = optionValue (args, "--mode");

        if (hasFlag (args, "--full"))
            mode = "full";

        if (hasFlag (args, "--interesting"))
            mode = "interesting";

        if (hasFlag (args, "--minimal"))
            mode = "minimal";

        if (mode.isNotEmpty())
            params.setProperty ("mode", mode);

        auto ref = optionValue (args, "--ref");

        if (ref.isNotEmpty())
            params.setProperty ("ref", ref);

        auto target = optionValue (args, "--target");

        if (target.isNotEmpty())
            params.setProperty ("target", target);

        auto since = optionValue (args, "--since");

        if (since.isNotEmpty())
            params.setProperty ("since", since);

        auto maxNodes = optionValue (args, "--max-nodes");

        if (maxNodes.isNotEmpty())
            params.setProperty ("maxNodes", maxNodes.getIntValue());

        auto maxChildren = optionValue (args, "--max-children");

        if (maxChildren.isNotEmpty())
            params.setProperty ("maxChildrenPerContainer", maxChildren.getIntValue());

        auto maxText = optionValue (args, "--max-text");

        if (maxText.isNotEmpty())
            params.setProperty ("maxTextLength", maxText.getIntValue());

        if (hasFlag (args, "--include-hidden"))
            params.setProperty ("includeHidden", true);

        if (hasFlag (args, "--exclude-disabled"))
            params.setProperty ("includeDisabled", false);

        if (hasFlag (args, "--no-actions"))
            params.setProperty ("includeActions", false);

        if (hasFlag (args, "--no-bounds"))
            params.setProperty ("includeBounds", false);
    }

    void addTimeoutOption (juce::StringArray& args, juce::DynamicObject& params)
    {
        auto timeout = optionValue (args, "--timeout-ms", optionValue (args, "--timeout"));

        if (timeout.isNotEmpty())
            params.setProperty ("timeoutMs", timeout.getIntValue());
    }

    void addActionOptions (juce::StringArray& args, juce::DynamicObject& params)
    {
        addTimeoutOption (args, params);

        if (hasFlag (args, "--force"))
            params.setProperty ("force", true);

        if (hasFlag (args, "--trial"))
            params.setProperty ("trial", true);
    }

    void copyActionOptions (juce::DynamicObject& source, juce::DynamicObject& destination)
    {
        for (auto name : { "timeoutMs", "force", "trial" })
        {
            const auto value = source.getProperty (name);

            if (! value.isVoid())
                destination.setProperty (name, value);
        }
    }

    void addActionOptionsAndLocator (juce::DynamicObject& params, juce::DynamicObject& actionOptions, const juce::var& locator)
    {
        copyActionOptions (actionOptions, params);
        addLocatorIfPresent (params, locator);
    }

    juce::var object (std::initializer_list<std::pair<juce::String, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();

        for (const auto& property : properties)
            result->setProperty (property.first, property.second);

        return result;
    }

    juce::var parsePositionOption (const juce::String& position)
    {
        if (position.isEmpty())
            return {};

        juce::StringArray coordinates;
        coordinates.addTokens (position, ",", {});
        coordinates.trim();

        if (coordinates.size() != 2)
            throw std::runtime_error ("--position must use x,y");

        return object ({ { "x", coordinates[0].getDoubleValue() },
                         { "y", coordinates[1].getDoubleValue() } });
    }

    void addPositionIfPresent (juce::DynamicObject& params, const juce::String& position)
    {
        auto parsed = parsePositionOption (position);

        if (! parsed.isVoid())
            params.setProperty ("position", parsed);
    }

    juce::var emptyObject()
    {
        return juce::var (new juce::DynamicObject());
    }

    juce::var array (std::initializer_list<juce::var> values)
    {
        juce::Array<juce::var> result;

        for (const auto& value : values)
            result.add (value);

        return result;
    }

    juce::var stringSchema()
    {
        return object ({ { "type", "string" } });
    }

    juce::var numberSchema()
    {
        return object ({ { "type", "number" } });
    }

    juce::var timeoutMsSchema()
    {
        return object ({ { "type", "number" }, { "default", 5000 } });
    }

    juce::var booleanSchema()
    {
        return object ({ { "type", "boolean" } });
    }

    juce::var valueSchema()
    {
        return object ({ { "anyOf", array ({ stringSchema(),
                                             numberSchema(),
                                             booleanSchema(),
                                             object ({ { "type", "object" } }),
                                             object ({ { "type", "array" } }) }) } });
    }

    juce::var stringArraySchema()
    {
        return object ({ { "type", "array" }, { "items", stringSchema() } });
    }

    juce::var locatorSchema()
    {
        return object ({ { "type", "object" },
                         { "properties", object ({ { "role", stringSchema() },
                                                   { "name", stringSchema() },
                                                   { "text", stringSchema() },
                                                   { "componentId", stringSchema() },
                                                   { "componentName", stringSchema() },
                                                   { "testId", stringSchema() },
                                                   { "class", stringSchema() },
                                                   { "value", stringSchema() },
                                                   { "hasText", stringSchema() },
                                                   { "exact", booleanSchema() },
                                                   { "visible", booleanSchema() },
                                                   { "accessible", booleanSchema() },
                                                   { "enabled", booleanSchema() },
                                                   { "focused", booleanSchema() },
                                                   { "selected", booleanSchema() },
                                                   { "nth", numberSchema() } }) } });
    }

    juce::var toolSchema (std::initializer_list<std::pair<juce::String, juce::var>> properties,
                          std::initializer_list<juce::var> required = {})
    {
        auto schema = object ({ { "type", "object" }, { "properties", object (properties) } });

        if (required.size() > 0)
            schema.getDynamicObject()->setProperty ("required", array (required));

        return schema;
    }

    juce::var timedToolSchema (std::initializer_list<std::pair<juce::String, juce::var>> properties,
                               std::initializer_list<juce::var> required = {})
    {
        auto schema = toolSchema (properties, required);

        if (auto* schemaObject = schema.getDynamicObject())
            if (auto* propertiesObject = schemaObject->getProperty ("properties").getDynamicObject())
                propertiesObject->setProperty ("timeoutMs", timeoutMsSchema());

        return schema;
    }

    juce::var targetActionToolSchema (std::initializer_list<std::pair<juce::String, juce::var>> properties,
                                      std::initializer_list<juce::var> required = {})
    {
        auto schema = timedToolSchema (properties, required);

        if (auto* schemaObject = schema.getDynamicObject())
        {
            if (auto* propertiesObject = schemaObject->getProperty ("properties").getDynamicObject())
            {
                propertiesObject->setProperty ("force", booleanSchema());
                propertiesObject->setProperty ("trial", booleanSchema());
            }
        }

        return schema;
    }

    juce::var tool (const juce::String& name, const juce::String& description, const juce::var& inputSchema)
    {
        return object ({ { "name", name }, { "description", description }, { "inputSchema", inputSchema } });
    }

    juce::var mcpTools()
    {
        return array ({
            tool ("juce_list_sessions",
                  "List running melatonin_inspector automation sessions.",
                  toolSchema ({})),
            tool ("juce_windows",
                  "List automation-owned JUCE windows for a session.",
                  toolSchema ({ { "session", stringSchema() } })),
            tool ("juce_trace_start",
                  "Start recording automation trace events.",
                  toolSchema ({ { "session", stringSchema() }, { "file", stringSchema() } })),
            tool ("juce_trace_stop",
                  "Stop recording automation trace events and write the trace artifact.",
                  toolSchema ({ { "session", stringSchema() } })),
            tool ("juce_capabilities",
                  "Return protocol, feature, and security capabilities for a running automation session.",
                  toolSchema ({ { "session", stringSchema() } })),
            tool ("juce_locator",
                  "Find visible JUCE components by Playwright-style locator fields. Pass visible=false to inspect hidden components.",
                  timedToolSchema ({ { "session", stringSchema() }, { "locator", locatorSchema() } }, { "locator" })),
            tool ("juce_count",
                  "Count visible JUCE components matching Playwright-style locator fields without returning the component tree.",
                  timedToolSchema ({ { "session", stringSchema() }, { "locator", locatorSchema() } }, { "locator" })),
            tool ("juce_describe",
                  "Describe one component by ref or strict locator, including compact state, ancestors, children, and action hints.",
                  toolSchema ({ { "session", stringSchema() },
                                { "ref", stringSchema() },
                                { "locator", locatorSchema() },
                                { "mode", object ({ { "type", "string" },
                                                     { "enum", array ({ "interesting", "full", "minimal" }) },
                                                     { "default", "interesting" } }) },
                                { "depth", object ({ { "type", "number" }, { "default", 2 } }) },
                                { "timeoutMs", timeoutMsSchema() },
                                { "includeHidden", booleanSchema() },
                                { "includeActions", booleanSchema() },
                                { "includeBounds", booleanSchema() } })),
            tool ("juce_snapshot",
                  "Return a token-efficient Playwright-style snapshot of a JUCE component tree. Defaults to mode=interesting and format=json.",
                  toolSchema ({ { "session", stringSchema() },
                                { "mode", object ({ { "type", "string" },
                                                     { "enum", array ({ "interesting", "full", "minimal" }) },
                                                     { "default", "interesting" } }) },
                                { "format", object ({ { "type", "string" },
                                                       { "enum", array ({ "text", "json" }) },
                                                       { "default", "json" } }) },
                                { "depth", object ({ { "type", "number" }, { "default", 8 } }) },
                                { "target", stringSchema() },
                                { "ref", stringSchema() },
                                { "locator", locatorSchema() },
                                { "since", stringSchema() },
                                { "includeHidden", booleanSchema() },
                                { "includeDisabled", booleanSchema() },
                                { "includeActions", booleanSchema() },
                                { "includeBounds", booleanSchema() },
                                { "maxNodes", numberSchema() },
                                { "maxChildrenPerContainer", numberSchema() },
                                { "maxTextLength", numberSchema() },
                                { "timeoutMs", timeoutMsSchema() } })),
            tool ("juce_screenshot",
                  "Capture a PNG screenshot of the root or a component ref.",
                  timedToolSchema ({ { "session", stringSchema() },
                                     { "target", object ({ { "type", "string" }, { "default", "root" } }) },
                                     { "ref", stringSchema() },
                                     { "locator", locatorSchema() },
                                     { "source", object ({ { "type", "string" },
                                                           { "enum", array ({ "auto", "component", "native" }) },
                                                           { "default", "auto" } }) },
                                     { "file", stringSchema() },
                                     { "clipX", numberSchema() },
                                     { "clipY", numberSchema() },
                                     { "clipW", numberSchema() },
                                     { "clipH", numberSchema() },
                                     { "scale", numberSchema() },
                                     { "includeBase64", booleanSchema() } })),
            tool ("juce_click",
                  "Click a component ref and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() },
                                            { "ref", stringSchema() },
                                            { "locator", locatorSchema() },
                                            { "button", stringSchema() },
                                            { "clickCount", numberSchema() },
                                            { "position", object ({ { "type", "object" },
                                                                    { "properties", object ({ { "x", numberSchema() }, { "y", numberSchema() } }) } }) } })),
            tool ("juce_dblclick",
                  "Double-click a component ref or locator and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() } })),
            tool ("juce_right_click",
                  "Right-click a component ref or locator and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "menuItem", stringSchema() } })),
            tool ("juce_click_xy",
                  "Click window-local coordinates and return a fresh snapshot.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_hover",
                  "Move the mouse to window-local coordinates.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_mouse_move",
                  "Move the mouse to window-local coordinates.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_mouse_down",
                  "Send a mouse-down event at window-local coordinates.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_mouse_up",
                  "Send a mouse-up event at window-local coordinates.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_wheel",
                  "Send a mouse wheel event at window-local coordinates.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() }, { "deltaX", numberSchema() }, { "deltaY", numberSchema() } }, { "x", "y" })),
            tool ("juce_drag_xy",
                  "Drag from one window-local point to another.",
                  timedToolSchema ({ { "session", stringSchema() }, { "target", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() }, { "toX", numberSchema() }, { "toY", numberSchema() }, { "steps", numberSchema() } }, { "x", "y", "toX", "toY" })),
            tool ("juce_type",
                  "Type text into a component ref and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "text", stringSchema() } }, { "text" })),
            tool ("juce_fill",
                  "Replace text in a TextEditor or Label.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "text", stringSchema() } }, { "text" })),
            tool ("juce_clear",
                  "Clear text from a TextEditor or Label.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() } })),
            tool ("juce_press",
                  "Press a key and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "key", stringSchema() } }, { "key" })),
            tool ("juce_key_down",
                  "Send a key-down event, supporting chords such as Control+K.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "key", stringSchema() } }, { "key" })),
            tool ("juce_key_up",
                  "Send a key-up event, supporting chords such as Control+K.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "key", stringSchema() } }, { "key" })),
            tool ("juce_check",
                  "Set a toggleable button checked.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() } })),
            tool ("juce_uncheck",
                  "Set a toggleable button unchecked.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() } })),
            tool ("juce_set_checked",
                  "Set a toggleable button checked or unchecked.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "checked", booleanSchema() } }, { "checked" })),
            tool ("juce_set_value",
                  "Set a semantic value on a Slider or TextEditor.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "value", numberSchema() } }, { "value" })),
            tool ("juce_select_option",
                  "Select a ComboBox option or ListBox row by text, index, or id.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "text", stringSchema() }, { "index", numberSchema() }, { "id", numberSchema() } })),
            tool ("juce_select_tab",
                  "Select a TabbedComponent tab by name or index.",
                  targetActionToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "name", stringSchema() }, { "index", numberSchema() } })),
            tool ("juce_drag",
                  "Drag a component by a delta and return a fresh snapshot.",
                  targetActionToolSchema ({ { "session", stringSchema() },
                                            { "ref", stringSchema() },
                                            { "locator", locatorSchema() },
                                            { "dx", numberSchema() },
                                            { "dy", numberSchema() },
                                            { "steps", numberSchema() },
                                            { "position", object ({ { "type", "object" },
                                                                    { "properties", object ({ { "x", numberSchema() }, { "y", numberSchema() } }) } }) } })),
            tool ("juce_drag_to",
                  "Drag a source component to a target component center.",
                  targetActionToolSchema ({ { "session", stringSchema() },
                                            { "ref", stringSchema() },
                                            { "locator", locatorSchema() },
                                            { "targetRef", stringSchema() },
                                            { "targetLocator", locatorSchema() },
                                            { "steps", numberSchema() } })),
            tool ("juce_drop",
                  "Invoke a JUCE DragAndDropTarget with a drag description.",
                  targetActionToolSchema ({ { "session", stringSchema() },
                                            { "ref", stringSchema() },
                                            { "locator", locatorSchema() },
                                            { "description", stringSchema() },
                                            { "sourceRef", stringSchema() },
                                            { "sourceLocator", locatorSchema() },
                                            { "position", object ({ { "type", "object" },
                                                                    { "properties", object ({ { "x", numberSchema() }, { "y", numberSchema() } }) } }) } },
                                          { "description" })),
            tool ("juce_drop_files",
                  "Invoke a JUCE FileDragAndDropTarget with one or more file paths.",
                  targetActionToolSchema ({ { "session", stringSchema() },
                                            { "ref", stringSchema() },
                                            { "locator", locatorSchema() },
                                            { "file", stringSchema() },
                                            { "files", stringArraySchema() },
                                            { "position", object ({ { "type", "object" },
                                                                    { "properties", object ({ { "x", numberSchema() }, { "y", numberSchema() } }) } }) } })),
            tool ("juce_set_bounds",
                  "Set a component's bounds and return a fresh snapshot.",
                  timedToolSchema ({ { "session", stringSchema() },
                                     { "ref", stringSchema() },
                                     { "locator", locatorSchema() },
                                     { "x", numberSchema() },
                                     { "y", numberSchema() },
                                     { "w", numberSchema() },
                                     { "h", numberSchema() } })),
            tool ("juce_set_property",
                  "Set a component property and return a fresh snapshot.",
                  timedToolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "name", stringSchema() }, { "value", valueSchema() } },
                                   { "name" })),
            tool ("juce_wait",
                  "Wait briefly and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ms", object ({ { "type", "number" }, { "default", 250 } }) } })),
            tool ("juce_wait_for_ref",
                  "Wait for a previously returned component ref to remain attached.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "timeoutMs", timeoutMsSchema() } }, { "ref" })),
            tool ("juce_wait_for_locator",
                  "Wait for a visible locator match unless the locator requests hidden components.",
                  toolSchema ({ { "session", stringSchema() }, { "locator", locatorSchema() }, { "timeoutMs", timeoutMsSchema() } }, { "locator" })),
            tool ("juce_wait_for_text",
                  "Wait for visible text in the component tree.",
                  toolSchema ({ { "session", stringSchema() }, { "text", stringSchema() }, { "timeoutMs", timeoutMsSchema() }, { "exact", booleanSchema() }, { "visible", booleanSchema() } }, { "text" })),
            tool ("juce_wait_for_value",
                  "Wait for a semantic component value to match.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "value", stringSchema() }, { "timeoutMs", timeoutMsSchema() }, { "exact", booleanSchema() } }, { "value" })),
            tool ("juce_wait_for_snapshot_change",
                  "Wait for the snapshot state hash to differ from a previous state hash.",
                  toolSchema ({ { "session", stringSchema() }, { "stateHash", stringSchema() }, { "timeoutMs", timeoutMsSchema() } }, { "stateHash" }))
        });
    }

    juce::String methodForTool (const juce::String& name)
    {
        if (name == "juce_snapshot") return "snapshot";
        if (name == "juce_capabilities") return "capabilities";
        if (name == "juce_locator") return "locator";
        if (name == "juce_count") return "count";
        if (name == "juce_describe") return "describe";
        if (name == "juce_screenshot") return "screenshot";
        if (name == "juce_click") return "click";
        if (name == "juce_dblclick") return "dblclick";
        if (name == "juce_right_click") return "right_click";
        if (name == "juce_click_xy") return "click_xy";
        if (name == "juce_hover") return "hover";
        if (name == "juce_mouse_move") return "mouse_move";
        if (name == "juce_mouse_down") return "mouse_down";
        if (name == "juce_mouse_up") return "mouse_up";
        if (name == "juce_wheel") return "wheel";
        if (name == "juce_drag_xy") return "drag_xy";
        if (name == "juce_type") return "type";
        if (name == "juce_fill") return "fill";
        if (name == "juce_clear") return "clear";
        if (name == "juce_press") return "press";
        if (name == "juce_key_down") return "key_down";
        if (name == "juce_key_up") return "key_up";
        if (name == "juce_check") return "check";
        if (name == "juce_uncheck") return "uncheck";
        if (name == "juce_set_checked") return "set_checked";
        if (name == "juce_set_value") return "set_value";
        if (name == "juce_select_option") return "select_option";
        if (name == "juce_select_tab") return "select_tab";
        if (name == "juce_drag") return "drag";
        if (name == "juce_drag_to") return "drag_to";
        if (name == "juce_drop") return "drop";
        if (name == "juce_drop_files") return "drop_files";
        if (name == "juce_set_bounds") return "set_bounds";
        if (name == "juce_set_property") return "set_property";
        if (name == "juce_wait") return "wait";
        if (name == "juce_wait_for_ref") return "wait_for_ref";
        if (name == "juce_wait_for_locator") return "wait_for_locator";
        if (name == "juce_wait_for_text") return "wait_for_text";
        if (name == "juce_wait_for_value") return "wait_for_value";
        if (name == "juce_wait_for_snapshot_change") return "wait_for_snapshot_change";
        if (name == "juce_windows") return "windows";
        if (name == "juce_trace_start") return "trace_start";
        if (name == "juce_trace_stop") return "trace_stop";

        return {};
    }

    juce::var mcpTextContent (const juce::String& text)
    {
        return object ({ { "content", array ({ object ({ { "type", "text" }, { "text", text } }) }) } });
    }

    juce::var mcpEndpointErrorContent (juce::DynamicObject& responseObject)
    {
        return object ({ { "isError", true },
                         { "content", array ({ object ({ { "type", "text" },
                                                          { "text", juce::JSON::toString (responseObject.getProperty ("error"), true) } }) }) } });
    }

    juce::var callMcpTool (const juce::String& name, juce::var arguments)
    {
        if (!arguments.isObject())
            arguments = emptyObject();

        auto* args = arguments.getDynamicObject();

        if (name == "juce_list_sessions")
        {
            juce::Array<juce::var> publicSessions;

            for (const auto& session : loadSessions())
            {
                if (auto* sessionObject = session.getDynamicObject())
                {
                    publicSessions.add (object ({ { "pid", sessionObject->getProperty ("pid") },
                                                  { "session", sessionObject->getProperty ("session") },
                                                  { "root", sessionObject->getProperty ("root") },
                                                  { "host", sessionObject->getProperty ("host") },
                                                  { "port", sessionObject->getProperty ("port") },
                                                  { "file", sessionObject->getProperty ("file") },
                                                  { "modifiedAtMs", sessionObject->getProperty ("modifiedAtMs") } }));
                }
            }

            return mcpTextContent (juce::JSON::toString (juce::var (publicSessions), true));
        }

        const auto sessionName = args->getProperty ("session").toString();
        auto session = findSession (sessionName);
        auto* sessionObject = session.getDynamicObject();

        if (sessionObject == nullptr)
            throw std::runtime_error (("No melatonin_inspector automation session found"
                                       + (sessionName.isNotEmpty() ? " for '" + sessionName + "'" : juce::String()))
                                          .toStdString());

        const auto method = methodForTool (name);

        if (method.isEmpty())
            throw std::runtime_error (("Unknown melatonin MCP tool: " + name).toStdString());

        if (name == "juce_snapshot")
        {
            if (args->getProperty ("format").isVoid())
                args->setProperty ("format", "json");

            if (args->getProperty ("mode").isVoid())
                args->setProperty ("mode", "interesting");
        }

        if (name == "juce_screenshot" && args->getProperty ("includeBase64").isVoid())
            args->setProperty ("includeBase64", true);

        auto response = requestEnvelope (*sessionObject, method, arguments);
        auto* responseObject = response.getDynamicObject();

        if (responseObject == nullptr)
            throw std::runtime_error ("Invalid response from automation endpoint");

        if (! (bool) responseObject->getProperty ("ok"))
            return mcpEndpointErrorContent (*responseObject);

        auto result = responseObject->getProperty ("result");

        if (name == "juce_screenshot")
        {
            juce::Array<juce::var> content;
            auto* resultObject = result.getDynamicObject();

            if (resultObject != nullptr)
            {
                auto file = resultObject->getProperty ("file").toString();
                auto base64 = resultObject->getProperty ("base64").toString();
                auto mimeType = resultObject->getProperty ("mimeType").toString();

                if (file.isNotEmpty())
                    content.add (object ({ { "type", "text" }, { "text", file } }));

                if (base64.isNotEmpty())
                    content.add (object ({ { "type", "image" },
                                           { "data", base64 },
                                           { "mimeType", mimeType.isNotEmpty() ? mimeType : juce::String ("image/png") } }));
            }

            return object ({ { "content", juce::var (content) } });
        }

        if (name == "juce_snapshot" && args->getProperty ("format").toString() == "json")
            return mcpTextContent (juce::JSON::toString (result, true));

        if (name == "juce_locator" || name == "juce_count" || name == "juce_describe")
            return mcpTextContent (juce::JSON::toString (result, true));

        if (auto* resultObject = result.getDynamicObject())
        {
            auto text = resultObject->getProperty ("text").toString();

            if (text.isNotEmpty())
                return mcpTextContent (text);
        }

        return mcpTextContent (juce::JSON::toString (result, true));
    }

    juce::var jsonRpcResult (const juce::var& id, const juce::var& result)
    {
        return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
    }

    juce::var jsonRpcError (const juce::var& id, int code, const juce::String& message)
    {
        return object ({ { "jsonrpc", "2.0" },
                         { "id", id },
                         { "error", object ({ { "code", code }, { "message", message } }) } });
    }

    juce::var handleMcpLine (const juce::String& line)
    {
        auto parsed = juce::JSON::parse (line);
        auto* requestObject = parsed.getDynamicObject();

        if (requestObject == nullptr)
            return jsonRpcError ({}, -32700, "Parse error");

        auto id = requestObject->getProperty ("id");
        auto method = requestObject->getProperty ("method").toString();

        try
        {
            if (method == "initialize")
            {
                auto params = requestObject->getProperty ("params");
                auto* paramsObject = params.getDynamicObject();
                auto protocolVersion = paramsObject != nullptr ? paramsObject->getProperty ("protocolVersion").toString() : juce::String();

                if (protocolVersion.isEmpty())
                    protocolVersion = "2025-11-25";

                return jsonRpcResult (id,
                                      object ({ { "protocolVersion", protocolVersion },
                                                { "capabilities", object ({ { "tools", emptyObject() } }) },
                                                { "serverInfo", object ({ { "name", "melatonin-mcp" }, { "version", "0.1.0" } }) } }));
            }

            if (method == "tools/list")
                return jsonRpcResult (id, object ({ { "tools", mcpTools() } }));

            if (method == "tools/call")
            {
                auto params = requestObject->getProperty ("params");
                auto* paramsObject = params.getDynamicObject();

                if (paramsObject == nullptr)
                    return jsonRpcError (id, -32602, "tools/call params must be an object");

                return jsonRpcResult (id,
                                      callMcpTool (paramsObject->getProperty ("name").toString(),
                                                   paramsObject->getProperty ("arguments")));
            }

            if (id.isVoid())
                return {};

            return jsonRpcError (id, -32601, "Method not found: " + method);
        }
        catch (const std::exception& e)
        {
            return jsonRpcError (id, -32000, e.what());
        }
    }

    void runMcpServer()
    {
        std::string line;

        while (std::getline (std::cin, line))
        {
            auto response = handleMcpLine (juce::String::fromUTF8 (line.data(), (int) line.size()));

            if (!response.isVoid())
                std::cout << juce::JSON::toString (response, true).toStdString() << "\n";
        }
    }

    void printResult (const juce::var& result, bool preferJson = false)
    {
        if (!preferJson)
        {
            if (auto* object = result.getDynamicObject())
            {
                auto text = object->getProperty ("text").toString();

                if (text.isNotEmpty())
                {
                    std::cout << text.toStdString();
                    std::cout.flush();
                    return;
                }

                auto file = object->getProperty ("file").toString();

                if (file.isNotEmpty())
                {
                    std::cout << file.toStdString() << "\n";
                    std::cout.flush();
                    return;
                }
            }
        }

        std::cout << juce::JSON::toString (result, true).toStdString() << "\n";
        std::cout.flush();
    }

    void printHelp()
    {
        std::cout
            << "Usage:\n"
            << "  melatonin-ui list\n"
            << "  melatonin-ui mcp\n"
            << "  melatonin-ui launch --app APP_OR_BUNDLE [--session NAME] [--artifact-dir DIR] [--home DIR] [--copy-setting FILE]\n"
            << "  melatonin-ui prepare-juce-profile --home DIR --app-name NAME [--source-home DIR] [--copy-setting FILE] [--keep-filter-state] [--keep-audio-state]\n"
            << "  melatonin-ui -s <session> capabilities\n"
            << "  melatonin-ui -s <session> windows\n"
            << "  melatonin-ui -s <session> trace-start --file trace.json\n"
            << "  melatonin-ui -s <session> trace-stop\n"
            << "  melatonin-ui -s <session> locator [--role role] [--name text] [--text text] [--selected] [--format json] [--timeout-ms n]\n"
            << "  melatonin-ui -s <session> count [locator options] [--timeout-ms n]\n"
            << "  melatonin-ui -s <session> describe <ref>|[locator options] [--depth n] [--full|--interesting|--minimal] [--timeout-ms n]\n"
            << "  melatonin-ui -s <session> snapshot [--json|--format text|json] [--full|--interesting|--minimal] [--depth n] [--ref ref] [locator options] [--timeout-ms n]\n"
            << "  melatonin-ui -s <session> screenshot [--target root|--ref m1-1] [--source auto|component|native] [--base64] [--timeout-ms n] [locator options] --file /tmp/root.png\n"
            << "  melatonin-ui -s <session> click <ref>|[locator options] [--button left|right|middle] [--click-count n] [--position x,y]\n"
            << "  melatonin-ui -s <session> dblclick <ref>|[locator options]\n"
            << "  melatonin-ui -s <session> right-click <ref>|[locator options] [--menu-item name]\n"
            << "  melatonin-ui -s <session> click-xy <x> <y> [--target root|window-1]\n"
            << "  melatonin-ui -s <session> hover <x> <y> [--target root|window-1]\n"
            << "  melatonin-ui -s <session> mouse-move <x> <y> [--target root|window-1]\n"
            << "  melatonin-ui -s <session> mouse-down <x> <y> [--target root|window-1]\n"
            << "  melatonin-ui -s <session> mouse-up <x> <y> [--target root|window-1]\n"
            << "  melatonin-ui -s <session> wheel <x> <y> --dy amount [--target root|window-1]\n"
            << "  melatonin-ui -s <session> drag-xy <x> <y> <toX> <toY> [--target root|window-1] [--steps n]\n"
            << "  melatonin-ui -s <session> type <ref>|[locator options] <text>\n"
            << "  melatonin-ui -s <session> fill <ref>|[locator options] <text>\n"
            << "  melatonin-ui -s <session> clear <ref>|[locator options]\n"
            << "  melatonin-ui -s <session> check <ref>|[locator options]\n"
            << "  melatonin-ui -s <session> uncheck <ref>|[locator options]\n"
            << "  melatonin-ui -s <session> set-checked <ref>|[locator options] true|false\n"
            << "  melatonin-ui -s <session> set-value <ref>|[locator options] <value>\n"
            << "  melatonin-ui -s <session> select-option <ref>|[locator options] --text name|--index n|--id n\n"
            << "  melatonin-ui -s <session> select-tab <ref>|[locator options] --name tab|--index n\n"
            << "  melatonin-ui -s <session> press <key> [--ref m1|locator options]\n"
            << "  melatonin-ui -s <session> key-down <key> [--ref m1]\n"
            << "  melatonin-ui -s <session> key-up <key> [--ref m1]\n"
            << "  melatonin-ui -s <session> drag <ref>|[locator options] --dx n --dy n [--steps n] [--position x,y]\n"
            << "  melatonin-ui -s <session> drag-to <ref>|[locator options] <target-ref>|[target locator options] [--steps n]\n"
            << "  melatonin-ui -s <session> drop <ref>|[locator options] --description text [--position x,y]\n"
            << "  melatonin-ui -s <session> drop-files <ref>|[locator options] --file path [--position x,y]\n"
            << "  melatonin-ui -s <session> set-bounds <ref> --x n --y n --w n --h n\n"
            << "  melatonin-ui -s <session> set-property <ref> <name> <value>\n"
            << "  melatonin-ui -s <session> wait --ms n\n"
            << "  melatonin-ui -s <session> wait-for-text <text> [--timeout-ms n]\n"
            << "\nSnapshot defaults to a compact interesting tree. Use --full for the complete component dump.\n"
            << "Screenshot base64 is off by default for CLI; use --base64 to include it.\n";
    }

    juce::String popFront (juce::StringArray& args)
    {
        if (args.isEmpty())
            return {};

        auto value = args[0];
        args.remove (0);
        return value;
    }
}

int main (int argc, char* argv[])
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.isEmpty() || hasFlag (args, "--help") || hasFlag (args, "-h"))
    {
        printHelp();
        return 0;
    }

    const auto sessionName = optionValue (args, "-s");
    auto command = popFront (args);

    if (command == "mcp")
    {
        runMcpServer();
        return 0;
    }

    if (command == "list")
    {
        for (auto& session : loadSessions())
        {
            if (auto* object = session.getDynamicObject())
            {
                std::cout << object->getProperty ("session").toString().toStdString()
                          << " pid=" << object->getProperty ("pid").toString().toStdString()
                          << " root=\"" << object->getProperty ("root").toString().toStdString()
                          << "\" port=" << object->getProperty ("port").toString().toStdString()
                          << "\n";
            }
        }

        return 0;
    }

    if (command == "prepare-juce-profile")
    {
        try
        {
            auto homePath = optionValue (args, "--home");
            auto sourceHomePath = optionValue (args, "--source-home", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName());
            auto appPath = optionValue (args, "--app");
            auto appName = optionValue (args, "--app-name", appNameFromPath (appPath));
            juce::StringArray extraSettings;

            for (;;)
            {
                auto setting = optionValue (args, "--copy-setting");

                if (setting.isEmpty())
                    break;

                extraSettings.addIfNotAlreadyThere (setting);
            }

            if (homePath.isEmpty())
                throw std::runtime_error ("prepare-juce-profile requires --home");

            auto result = prepareJuceProfile (juce::File::createFileWithoutCheckingPath (homePath),
                                              juce::File::createFileWithoutCheckingPath (sourceHomePath),
                                              appName,
                                              extraSettings,
                                              ! hasFlag (args, "--keep-filter-state"),
                                              ! hasFlag (args, "--keep-audio-state"));
            printResult (result, true);
            return 0;
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    if (command == "launch")
    {
        try
        {
            printResult (launchAutomationApp (args), true);
            return 0;
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    auto session = findSession (sessionName);
    auto* sessionObject = session.getDynamicObject();

    if (sessionObject == nullptr)
    {
        std::cerr << "No melatonin_inspector automation session found";

        if (sessionName.isNotEmpty())
            std::cerr << " for '" << sessionName.toStdString() << "'";

        std::cerr << ".\n";
        return 1;
    }

    try
    {
        if (command == "snapshot")
        {
            auto format = hasFlag (args, "--json") ? juce::String ("json") : optionValue (args, "--format", "text");
            auto depth = optionValue (args, "--depth", "8").getIntValue();
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "format", format }, { "depth", depth } });
            addSnapshotOptions (args, *params.getDynamicObject());
            addTimeoutOption (args, *params.getDynamicObject());
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            auto result = request (*sessionObject, "snapshot", params);
            printResult (result, format == "json");
            return 0;
        }

        if (command == "locator")
        {
            auto format = optionValue (args, "--format", "json");
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "locator", locator } });
            addTimeoutOption (args, *params.getDynamicObject());
            printResult (request (*sessionObject, "locator", params), format == "json");
            return 0;
        }

        if (command == "count")
        {
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "locator", locator } });
            addTimeoutOption (args, *params.getDynamicObject());
            printResult (request (*sessionObject, "count", params), true);
            return 0;
        }

        if (command == "describe")
        {
            auto format = hasFlag (args, "--json") ? juce::String ("json") : optionValue (args, "--format", "json");
            auto depth = optionValue (args, "--depth");
            auto locator = parseLocatorOptions (args);
            auto params = object ({});
            addSnapshotOptions (args, *params.getDynamicObject());
            addTimeoutOption (args, *params.getDynamicObject());

            if (depth.isNotEmpty())
                params.getDynamicObject()->setProperty ("depth", depth.getIntValue());

            if (!locator.isVoid())
            {
                params.getDynamicObject()->setProperty ("locator", locator);
            }
            else if (!args.isEmpty())
            {
                params.getDynamicObject()->setProperty ("ref", args[0]);
            }

            auto result = request (*sessionObject, "describe", params);
            printResult (result, format == "json");
            return 0;
        }

        if (command == "capabilities")
        {
            printResult (request (*sessionObject, "capabilities", emptyObject()), true);
            return 0;
        }

        if (command == "windows")
        {
            printResult (request (*sessionObject, "windows", emptyObject()), true);
            return 0;
        }

        if (command == "trace-start")
        {
            printResult (request (*sessionObject, "trace_start", object ({ { "file", optionValue (args, "--file") } })), true);
            return 0;
        }

        if (command == "trace-stop")
        {
            printResult (request (*sessionObject, "trace_stop", emptyObject()), true);
            return 0;
        }

        if (command == "screenshot")
        {
            auto file = optionValue (args, "--file", optionValue (args, "--path"));
            auto ref = optionValue (args, "--ref");
            auto target = optionValue (args, "--target", "root");
            auto source = optionValue (args, "--source");
            auto clipX = optionValue (args, "--clip-x");
            auto clipY = optionValue (args, "--clip-y");
            auto clipW = optionValue (args, "--clip-w");
            auto clipH = optionValue (args, "--clip-h");
            auto scale = optionValue (args, "--scale");
            auto includeBase64 = hasFlag (args, "--base64");
            includeBase64 = hasFlag (args, "--no-base64") ? false : includeBase64;
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "file", file }, { "ref", ref }, { "target", target } });

            if (source.isNotEmpty()) params.getDynamicObject()->setProperty ("source", source);
            if (clipX.isNotEmpty()) params.getDynamicObject()->setProperty ("clipX", clipX.getIntValue());
            if (clipY.isNotEmpty()) params.getDynamicObject()->setProperty ("clipY", clipY.getIntValue());
            if (clipW.isNotEmpty()) params.getDynamicObject()->setProperty ("clipW", clipW.getIntValue());
            if (clipH.isNotEmpty()) params.getDynamicObject()->setProperty ("clipH", clipH.getIntValue());
            if (scale.isNotEmpty()) params.getDynamicObject()->setProperty ("scale", scale.getDoubleValue());

            params.getDynamicObject()->setProperty ("includeBase64", includeBase64);
            addActionOptions (args, *params.getDynamicObject());
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            auto result = request (*sessionObject, "screenshot", params);
            printResult (result);
            return 0;
        }

        if (command == "click" || command == "dblclick" || command == "right-click")
        {
            auto button = optionValue (args, "--button");
            auto clickCount = optionValue (args, "--click-count");
            auto position = optionValue (args, "--position");
            auto menuItem = optionValue (args, "--menu-item");
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : (args.size() >= 1 ? args[0] : juce::String());
            auto params = object ({ { "ref", ref } });

            if (button.isNotEmpty())
                params.getDynamicObject()->setProperty ("button", button);

            if (clickCount.isNotEmpty())
                params.getDynamicObject()->setProperty ("clickCount", clickCount.getIntValue());

            if (menuItem.isNotEmpty())
                params.getDynamicObject()->setProperty ("menuItem", menuItem);

            addPositionIfPresent (*params.getDynamicObject(), position);
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject,
                                  command == "click" ? "click" : (command == "dblclick" ? "dblclick" : "right_click"),
                                  params));
            return 0;
        }

        if (command == "click-xy" && args.size() >= 2)
        {
            auto target = optionValue (args, "--target", "root");
            printResult (request (*sessionObject, "click_xy", object ({ { "target", target }, { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() } })));
            return 0;
        }

        if ((command == "hover" || command == "mouse-move") && args.size() >= 2)
        {
            auto target = optionValue (args, "--target", "root");
            printResult (request (*sessionObject, command == "hover" ? "hover" : "mouse_move", object ({ { "target", target }, { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() } })));
            return 0;
        }

        if ((command == "mouse-down" || command == "mouse-up") && args.size() >= 2)
        {
            auto target = optionValue (args, "--target", "root");
            printResult (request (*sessionObject, command == "mouse-down" ? "mouse_down" : "mouse_up", object ({ { "target", target }, { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() } })));
            return 0;
        }

        if (command == "wheel" && args.size() >= 2)
        {
            auto target = optionValue (args, "--target", "root");
            auto dx = optionValue (args, "--dx", "0").getDoubleValue();
            auto dy = optionValue (args, "--dy", "0").getDoubleValue();
            printResult (request (*sessionObject, "wheel", object ({ { "target", target }, { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() }, { "deltaX", dx }, { "deltaY", dy } })));
            return 0;
        }

        if (command == "drag-xy" && args.size() >= 4)
        {
            auto target = optionValue (args, "--target", "root");
            auto steps = optionValue (args, "--steps");
            auto params = object ({ { "target", target },
                                    { "x", args[0].getIntValue() },
                                    { "y", args[1].getIntValue() },
                                    { "toX", args[2].getIntValue() },
                                    { "toY", args[3].getIntValue() } });

            if (steps.isNotEmpty())
                params.getDynamicObject()->setProperty ("steps", steps.getIntValue());

            printResult (request (*sessionObject, "drag_xy", params));
            return 0;
        }

        if (command == "type")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);
            auto params = object ({ { "ref", ref }, { "text", args.joinIntoString (" ") } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "type", params));
            return 0;
        }

        if (command == "fill" || command == "clear")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);
            auto params = object ({ { "ref", ref }, { "text", command == "fill" ? args.joinIntoString (" ") : juce::String() } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, command == "fill" ? "fill" : "clear", params));
            return 0;
        }

        if (command == "check" || command == "uncheck" || command == "set-checked")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);
            auto params = object ({ { "ref", ref } });

            if (command == "set-checked")
            {
                if (args.isEmpty())
                    throw std::runtime_error ("set-checked requires true or false");

                params.getDynamicObject()->setProperty ("checked", (bool) parseValue (args[0]));
            }

            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject,
                                  command == "check" ? "check" : (command == "uncheck" ? "uncheck" : "set_checked"),
                                  params));
            return 0;
        }

        if (command == "set-value")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);

            if (args.isEmpty())
                throw std::runtime_error ("set-value requires a value");

            auto params = object ({ { "ref", ref }, { "value", parseValue (args[0]) } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "set_value", params));
            return 0;
        }

        if (command == "select-option")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto text = optionValue (args, "--text");
            auto index = optionValue (args, "--index");
            auto id = optionValue (args, "--id");
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : (args.size() >= 1 ? args[0] : juce::String());
            auto params = object ({ { "ref", ref },
                                    { "text", text } });

            if (index.isNotEmpty())
                params.getDynamicObject()->setProperty ("index", index.getIntValue());

            if (id.isNotEmpty())
                params.getDynamicObject()->setProperty ("id", id.getIntValue());

            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "select_option", params));
            return 0;
        }

        if (command == "select-tab")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto name = optionValue (args, "--name");
            auto index = optionValue (args, "--index");
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : (args.size() >= 1 ? args[0] : juce::String());
            auto params = object ({ { "ref", ref },
                                    { "name", name } });

            if (index.isNotEmpty())
                params.getDynamicObject()->setProperty ("index", index.getIntValue());

            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "select_tab", params));
            return 0;
        }

        if ((command == "press" || command == "key-down" || command == "key-up") && args.size() >= 1)
        {
            auto ref = optionValue (args, "--ref");
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "key", args[0] }, { "ref", ref } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject,
                                  command == "press" ? "press" : (command == "key-down" ? "key_down" : "key_up"),
                                  params));
            return 0;
        }

        if (command == "drag-to")
        {
            auto steps = optionValue (args, "--steps");
            auto targetRef = optionValue (args, "--target-ref");
            auto targetLocator = parseTargetLocatorOptions (args);
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto sourceRef = !locator.isVoid() ? juce::String() : popFront (args);

            if (targetRef.isEmpty() && targetLocator.isVoid() && !args.isEmpty())
                targetRef = popFront (args);

            auto params = object ({ { "ref", sourceRef }, { "targetRef", targetRef } });

            if (steps.isNotEmpty())
                params.getDynamicObject()->setProperty ("steps", steps.getIntValue());

            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);

            if (!targetLocator.isVoid())
                params.getDynamicObject()->setProperty ("targetLocator", targetLocator);

            printResult (request (*sessionObject, "drag_to", params));
            return 0;
        }

        if (command == "drop")
        {
            auto description = optionValue (args, "--description");
            auto position = optionValue (args, "--position");
            auto sourceRef = optionValue (args, "--source-ref");
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);

            if (description.isEmpty())
                throw std::runtime_error ("drop requires --description");

            auto params = object ({ { "ref", ref }, { "description", description } });
            addPositionIfPresent (*params.getDynamicObject(), position);

            if (sourceRef.isNotEmpty())
                params.getDynamicObject()->setProperty ("sourceRef", sourceRef);

            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "drop", params));
            return 0;
        }

        if (command == "drop-files")
        {
            juce::Array<juce::var> files;

            for (;;)
            {
                auto file = optionValue (args, "--file");

                if (file.isEmpty())
                    break;

                files.add (file);
            }

            auto filesList = optionValue (args, "--files");
            if (filesList.isNotEmpty())
            {
                juce::StringArray parsed;
                parsed.addTokens (filesList, ",", "\"'");
                parsed.trim();
                parsed.removeEmptyStrings();

                for (const auto& file : parsed)
                    files.add (file);
            }

            auto position = optionValue (args, "--position");
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);

            if (files.isEmpty())
                throw std::runtime_error ("drop-files requires --file or --files");

            auto params = object ({ { "ref", ref }, { "files", juce::var (files) } });
            addPositionIfPresent (*params.getDynamicObject(), position);
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "drop_files", params));
            return 0;
        }

        if (command == "drag")
        {
            auto dx = optionValue (args, "--dx", "0").getIntValue();
            auto dy = optionValue (args, "--dy", "0").getIntValue();
            auto steps = optionValue (args, "--steps");
            auto position = optionValue (args, "--position");
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : (args.size() >= 1 ? args[0] : juce::String());
            auto params = object ({ { "ref", ref }, { "dx", dx }, { "dy", dy } });

            if (steps.isNotEmpty())
                params.getDynamicObject()->setProperty ("steps", steps.getIntValue());

            addPositionIfPresent (*params.getDynamicObject(), position);
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "drag", params));
            return 0;
        }

        if (command == "set-bounds")
        {
            auto x = optionValue (args, "--x", "0").getIntValue();
            auto y = optionValue (args, "--y", "0").getIntValue();
            auto w = optionValue (args, "--w", "0").getIntValue();
            auto h = optionValue (args, "--h", "0").getIntValue();
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : (args.size() >= 1 ? args[0] : juce::String());
            auto params = object ({ { "ref", ref }, { "x", x }, { "y", y }, { "w", w }, { "h", h } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "set_bounds", params));
            return 0;
        }

        if (command == "set-property")
        {
            juce::DynamicObject tempParams;
            addActionOptions (args, tempParams);
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);

            if (args.size() < 2)
                throw std::runtime_error ("set-property requires a property name and value");

            auto params = object ({ { "ref", ref }, { "name", args[0] }, { "value", parseValue (args[1]) } });
            addActionOptionsAndLocator (*params.getDynamicObject(), tempParams, locator);
            printResult (request (*sessionObject, "set_property", params));
            return 0;
        }

        if (command == "wait")
        {
            printResult (request (*sessionObject, "wait", object ({ { "ms", optionValue (args, "--ms", "250").getIntValue() } })));
            return 0;
        }

        if (command == "wait-for-ref")
        {
            printResult (request (*sessionObject, "wait_for_ref", object ({ { "ref", args.size() >= 1 ? args[0] : juce::String() },
                                                                            { "timeoutMs", optionValue (args, "--timeout-ms", "5000").getIntValue() } })));
            return 0;
        }

        if (command == "wait-for-locator")
        {
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "locator", locator },
                                    { "timeoutMs", optionValue (args, "--timeout-ms", "5000").getIntValue() } });
            printResult (request (*sessionObject, "wait_for_locator", params), true);
            return 0;
        }

        if (command == "wait-for-text")
        {
            auto timeout = optionValue (args, "--timeout-ms", "5000").getIntValue();
            auto depth = optionValue (args, "--depth");
            auto exact = hasFlag (args, "--exact");
            auto hidden = hasFlag (args, "--hidden");
            auto visible = hasFlag (args, "--visible");
            auto params = object ({ { "text", args.joinIntoString (" ") },
                                    { "timeoutMs", timeout } });

            if (depth.isNotEmpty())
                params.getDynamicObject()->setProperty ("depth", depth.getIntValue());

            if (exact)
                params.getDynamicObject()->setProperty ("exact", true);

            if (hidden)
                params.getDynamicObject()->setProperty ("visible", false);
            else if (visible)
                params.getDynamicObject()->setProperty ("visible", true);

            printResult (request (*sessionObject, "wait_for_text", params));
            return 0;
        }

        if (command == "wait-for-value")
        {
            auto value = optionValue (args, "--value");
            auto ref = optionValue (args, "--ref");
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "ref", ref },
                                    { "value", value },
                                    { "timeoutMs", optionValue (args, "--timeout-ms", "5000").getIntValue() } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "wait_for_value", params), true);
            return 0;
        }

        if (command == "wait-for-snapshot-change")
        {
            printResult (request (*sessionObject,
                                  "wait_for_snapshot_change",
                                  object ({ { "stateHash", optionValue (args, "--state-hash") },
                                            { "timeoutMs", optionValue (args, "--timeout-ms", "5000").getIntValue() } })));
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }

    printHelp();
    return 1;
}
