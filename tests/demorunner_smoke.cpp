#include <JuceHeader.h>

#include <iostream>
#include <stdexcept>

#ifndef MELATONIN_DEMORUNNER_EXECUTABLE
    #error "MELATONIN_DEMORUNNER_EXECUTABLE must point at the instrumented DemoRunner executable"
#endif

#ifndef MELATONIN_UI_EXECUTABLE
    #error "MELATONIN_UI_EXECUTABLE must point at the melatonin-ui executable"
#endif

namespace
{
    constexpr const char* sessionName = "juce_demorunner";

    juce::File tempDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory);
    }

    juce::File sessionsDirectory()
    {
        const auto temp = juce::SystemStats::getEnvironmentVariable (
        #if JUCE_WINDOWS
            "TEMP",
        #else
            "TMPDIR",
        #endif
            tempDirectory().getFullPathName());

        return juce::File (temp).getChildFile ("melatonin_inspector").getChildFile ("sessions");
    }

    [[noreturn]] void fail (const juce::String& message)
    {
        throw std::runtime_error (message.toStdString());
    }

    void require (bool condition, const juce::String& message)
    {
        if (!condition)
            fail (message);
    }

    juce::String shellQuote (const juce::String& text)
    {
        return "'" + text.replace ("'", "'\\''") + "'";
    }

    juce::StringArray makeArgs (std::initializer_list<juce::String> values)
    {
        juce::StringArray result;

        for (const auto& value : values)
            result.add (value);

        return result;
    }

    juce::DynamicObject& asObject (const juce::var& value, const juce::String& label)
    {
        auto* object = value.getDynamicObject();
        require (object != nullptr, label + " is not an object: " + juce::JSON::toString (value, true));
        return *object;
    }

    class DemoRunnerSmoke
    {
    public:
        DemoRunnerSmoke()
            : demoRunnerPath (juce::String (MELATONIN_DEMORUNNER_EXECUTABLE)),
              cliPath (juce::String (MELATONIN_UI_EXECUTABLE))
        {
            const auto artifactDir = juce::SystemStats::getEnvironmentVariable ("MELATONIN_DEMORUNNER_ARTIFACT_DIR", {});
            evidenceDirectory = artifactDir.isNotEmpty()
                                    ? juce::File (artifactDir)
                                    : tempDirectory().getChildFile ("melatonin-demorunner-smoke");
        }

        ~DemoRunnerSmoke()
        {
            stopDemoRunner();
        }

        void run()
        {
            require (demoRunnerPath.existsAsFile(), "Missing DemoRunner executable: " + demoRunnerPath.getFullPathName());
            require (cliPath.existsAsFile(), "Missing melatonin-ui executable: " + cliPath.getFullPathName());
            require (evidenceDirectory.createDirectory(), "Could not create evidence directory: " + evidenceDirectory.getFullPathName());
            clearEvidenceDirectory();

            cleanupSessionFiles();
            startDemoRunner();
            waitForSession();
            runCli ({ "list" });
            runCli ({ "-s", sessionName, "trace-start", "--file", "demorunner-trace.json" });

            auto startupSnapshot = runCli ({ "-s", sessionName, "snapshot", "--format", "text", "--depth", "8" });
            require (startupSnapshot.contains ("JUCE Logo"), "startup snapshot did not include the DemoRunner home page");
            captureScreenshot ("startup.png");

            runMcpSmoke();

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Browse Demos" });
            juce::Thread::sleep (500);
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", "GUI", "--timeout-ms", "2000" });
            captureScreenshot ("side-panel.png");

            runCli ({ "-s", sessionName, "click-xy", "70", "20" });
            juce::Thread::sleep (500);
            runCli ({ "-s", sessionName, "wait-for-text", "Graphics", "--timeout-ms", "2000" });
            captureScreenshot ("settings.png");

            runCli ({ "-s", sessionName, "click-xy", "30", "20" });
            juce::Thread::sleep (500);
            runCli ({ "-s", sessionName, "wait-for-text", "JUCE Logo", "--timeout-ms", "2000" });
            captureScreenshot ("home.png");

            auto traceStop = juce::JSON::parse (runCli ({ "-s", sessionName, "trace-stop" }));
            auto& traceStopObject = asObject (traceStop, "trace-stop");
            require ((int) traceStopObject.getProperty ("events") >= 4, "DemoRunner trace did not record enough events");
            copyEvidenceFile (traceStopObject.getProperty ("trace").toString(), "demorunner-trace.json");
        }

    private:
        juce::File demoRunnerPath;
        juce::File cliPath;
        juce::File evidenceDirectory;
        juce::ChildProcess demoRunner;

        void clearEvidenceDirectory()
        {
            for (juce::RangedDirectoryIterator entry (evidenceDirectory, false, "*", juce::File::findFiles);
                 entry != juce::RangedDirectoryIterator();
                 ++entry)
            {
                entry->getFile().deleteFile();
            }
        }

        void cleanupSessionFiles()
        {
            auto sessionDir = sessionsDirectory();

            if (!sessionDir.isDirectory())
                return;

            for (juce::RangedDirectoryIterator entry (sessionDir, false, "*" + juce::String (sessionName) + "*.json", juce::File::findFiles);
                 entry != juce::RangedDirectoryIterator();
                 ++entry)
            {
                entry->getFile().deleteFile();
            }
        }

        void startDemoRunner()
        {
            require (demoRunner.start (makeArgs ({ demoRunnerPath.getFullPathName() })),
                     "Could not launch DemoRunner: " + demoRunnerPath.getFullPathName());
        }

        void stopDemoRunner()
        {
            if (demoRunner.isRunning())
                demoRunner.kill();
        }

        void waitForSession()
        {
            const auto deadline = juce::Time::currentTimeMillis() + 20000;

            while (juce::Time::currentTimeMillis() < deadline)
            {
                if (!demoRunner.isRunning())
                    fail ("DemoRunner exited before advertising automation");

                auto result = runProcess (makeCliCommand ({ "-s", sessionName, "capabilities" }), "melatonin-ui capabilities", false, 3000);

                if (result.exitCode == 0)
                    return;

                juce::Thread::sleep (250);
            }

            fail ("Timed out waiting for DemoRunner automation session");
        }

        struct ProcessResult
        {
            int exitCode = -1;
            juce::String output;
        };

        juce::StringArray makeCliCommand (std::initializer_list<juce::String> args) const
        {
            auto command = makeArgs ({ cliPath.getFullPathName() });

            for (const auto& arg : args)
                command.add (arg);

            return command;
        }

        juce::String runCli (std::initializer_list<juce::String> args)
        {
            auto result = runProcess (makeCliCommand (args), "melatonin-ui " + makeArgs (args).joinIntoString (" "), true, 15000);
            return result.output.trim();
        }

        ProcessResult runProcess (const juce::StringArray& command, const juce::String& label, bool expectSuccess, int timeoutMs)
        {
            juce::ChildProcess process;
            ProcessResult result;
            require (process.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr),
                     "Could not start " + command.joinIntoString (" "));

            const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
            char buffer[4096] {};

            while (process.isRunning())
            {
                if (auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer)); bytesRead > 0)
                    result.output << juce::String::fromUTF8 (buffer, bytesRead);

                if (juce::Time::currentTimeMillis() > deadline)
                {
                    process.kill();
                    fail ("Timed out running " + label + "\n" + result.output);
                }

                juce::Thread::sleep (10);
            }

            for (;;)
            {
                const auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer));

                if (bytesRead <= 0)
                    break;

                result.output << juce::String::fromUTF8 (buffer, bytesRead);
            }

            result.exitCode = static_cast<int> (process.getExitCode());

            if (expectSuccess)
                require (result.exitCode == 0, label + " failed with exit code " + juce::String (result.exitCode) + "\n" + result.output);

            return result;
        }

        void captureScreenshot (const juce::String& name)
        {
            auto outputPath = runCli ({ "-s", sessionName, "screenshot", "--target", "root", "--file", name, "--no-base64" });
            auto screenshot = juce::File (outputPath);
            require (screenshot.existsAsFile() && screenshot.getSize() > 1000,
                     "Screenshot was not written or is too small: " + outputPath);
            copyEvidenceFile (outputPath, name);
        }

        void copyEvidenceFile (const juce::String& sourcePath, const juce::String& evidenceName)
        {
            auto source = juce::File (sourcePath);
            auto destination = evidenceDirectory.getChildFile (evidenceName);
            destination.deleteFile();
            require (source.copyFileTo (destination), "Could not copy evidence file: " + source.getFullPathName());
        }

        juce::String runMcpBatch (std::initializer_list<juce::String> requests)
        {
            auto requestFile = tempDirectory().getNonexistentChildFile ("melatonin-demorunner-mcp", ".jsonl");
            juce::String requestText;

            for (const auto& request : requests)
                requestText << request << "\n";

            require (requestFile.replaceWithText (requestText), "Could not write MCP request file: " + requestFile.getFullPathName());

            juce::StringArray command;

        #if JUCE_WINDOWS
            command.add ("cmd");
            command.add ("/C");
            command.add ("type " + shellQuote (requestFile.getFullPathName()) + " | " + shellQuote (cliPath.getFullPathName()) + " mcp");
        #else
            command.add ("/bin/sh");
            command.add ("-c");
            command.add ("cat " + shellQuote (requestFile.getFullPathName()) + " | " + shellQuote (cliPath.getFullPathName()) + " mcp");
        #endif

            auto output = runProcess (command, "melatonin-ui mcp", true, 15000).output;
            requestFile.deleteFile();
            return output;
        }

        juce::var parseMcpLine (const juce::StringArray& lines, int index)
        {
            require (juce::isPositiveAndBelow (index, lines.size()), "MCP response " + juce::String (index) + " missing");
            auto parsed = juce::JSON::parse (lines[index]);
            asObject (parsed, "MCP response " + juce::String (index));
            return parsed;
        }

        juce::var assertMcpResult (const juce::var& response, int expectedId)
        {
            auto& responseObject = asObject (response, "MCP response");
            require ((int) responseObject.getProperty ("id") == expectedId,
                     "MCP response id mismatch: " + juce::JSON::toString (response, true));
            require (responseObject.getProperty ("error").isVoid(),
                     "MCP returned an error: " + juce::JSON::toString (response, true));
            return responseObject.getProperty ("result");
        }

        void runMcpSmoke()
        {
            auto output = runMcpBatch ({
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})",
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"juce_snapshot","arguments":{"session":"juce_demorunner","format":"text","depth":5}}})",
                R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"juce_screenshot","arguments":{"session":"juce_demorunner","target":"root","includeBase64":true}}})"
            });

            auto lines = juce::StringArray::fromLines (output);
            require (lines.size() >= 4, "MCP smoke expected at least 4 response lines, got " + juce::String (lines.size()));

            assertMcpResult (parseMcpLine (lines, 0), 1);

            auto toolsList = assertMcpResult (parseMcpLine (lines, 1), 2);
            auto tools = asObject (toolsList, "MCP tools/list").getProperty ("tools");
            require (tools.isArray() && tools.getArray()->size() >= 10, "MCP tools/list returned too few tools");

            auto snapshotResult = assertMcpResult (parseMcpLine (lines, 2), 3);
            auto snapshotContent = asObject (snapshotResult, "MCP snapshot").getProperty ("content");
            require (snapshotContent.isArray() && !snapshotContent.getArray()->isEmpty(),
                     "MCP snapshot did not return content");
            auto snapshotText = asObject (snapshotContent.getArray()->getReference (0), "MCP snapshot content").getProperty ("text").toString();
            require (snapshotText.contains ("JUCE Logo"),
                     "MCP snapshot did not include JUCE Logo");

            auto screenshotResult = assertMcpResult (parseMcpLine (lines, 3), 4);
            auto screenshotContent = asObject (screenshotResult, "MCP screenshot").getProperty ("content");
            require (screenshotContent.isArray(), "MCP screenshot did not return content");

            bool foundImage = false;

            for (const auto& item : *screenshotContent.getArray())
            {
                auto& contentItem = asObject (item, "MCP screenshot content");
                foundImage = foundImage
                             || (contentItem.getProperty ("type").toString() == "image"
                                 && contentItem.getProperty ("mimeType").toString() == "image/png"
                                 && contentItem.getProperty ("data").toString().length() > 1000);
            }

            require (foundImage, "MCP screenshot did not return PNG image content");
        }
    };
}

int main()
{
    try
    {
        DemoRunnerSmoke().run();
        std::cout << "ok - DemoRunner automation smoke passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "DemoRunner automation smoke failed: " << e.what() << "\n";
        return 1;
    }
}
