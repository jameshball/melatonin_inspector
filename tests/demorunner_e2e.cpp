#include <JuceHeader.h>

#include <functional>
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

    class DemoRunnerE2E
    {
    public:
        DemoRunnerE2E()
            : demoRunnerPath (juce::String (MELATONIN_DEMORUNNER_EXECUTABLE)),
              cliPath (juce::String (MELATONIN_UI_EXECUTABLE))
        {
            const auto artifactDir = juce::SystemStats::getEnvironmentVariable ("MELATONIN_DEMORUNNER_ARTIFACT_DIR", {});
            evidenceDirectory = artifactDir.isNotEmpty()
                                    ? juce::File (artifactDir)
                                    : tempDirectory().getChildFile ("melatonin-demorunner-e2e");
        }

        ~DemoRunnerE2E()
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

            openDemosPanel();
            clickVisibleListItem ("GUI");
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", "AccessibilityDemo.h", "--exact", "--timeout-ms", "3000" });
            captureScreenshot ("gui-category.png");

            clickVisibleListItem ("AccessibilityDemo.h");
            runCli ({ "-s", sessionName, "wait-for-text", "Accessibility Demo", "--timeout-ms", "3000" });
            captureScreenshot ("accessibility-demo.png");
            exerciseAccessibilityDemo();

            selectTopLevelTab ("Code");
            runCli ({ "-s", sessionName, "wait-for-text", "CodeContent", "--timeout-ms", "3000" });
            captureScreenshot ("accessibility-code.png");

            selectTopLevelTab ("Demo");
            runCli ({ "-s", sessionName, "wait-for-text", "Accessibility Demo", "--timeout-ms", "3000" });

            openDemosPanel();
            clickVisibleListItem ("FlexBoxDemo.h");
            runCli ({ "-s", sessionName, "wait-for-text", "flex-grow", "--timeout-ms", "3000" });
            captureScreenshot ("flexbox-demo.png");
            exerciseFlexBoxDemo();

            selectTopLevelTab ("Settings");
            runCli ({ "-s", sessionName, "wait-for-text", "LookAndFeel:", "--timeout-ms", "3000" });
            captureScreenshot ("settings.png");
            exerciseSettings();

            openDemosPanel();
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Home", "--exact" });
            runCli ({ "-s", sessionName, "wait-for-text", "JUCE Logo", "--timeout-ms", "3000" });
            captureScreenshot ("home.png");

            auto traceStop = juce::JSON::parse (runCli ({ "-s", sessionName, "trace-stop" }));
            auto& traceStopObject = asObject (traceStop, "trace-stop");
            require ((int) traceStopObject.getProperty ("events") >= 20, "DemoRunner trace did not record enough events");
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
            return runCli (makeArgs (args));
        }

        juce::String runCli (juce::StringArray args)
        {
            juce::StringArray command;
            command.add (cliPath.getFullPathName());
            command.addArray (args);

            auto result = runProcess (command, "melatonin-ui " + args.joinIntoString (" "), true, 15000);
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

        juce::var readSnapshot (int depth = 4)
        {
            auto parsed = juce::JSON::parse (runCli ({ "-s", sessionName, "snapshot", "--format", "json", "--depth", juce::String (depth) }));
            asObject (parsed, "snapshot");
            return parsed;
        }

        juce::var readLocator (std::initializer_list<juce::String> locatorArgs)
        {
            auto args = makeArgs ({ "-s", sessionName, "locator", "--format", "json" });
            args.addArray (makeArgs (locatorArgs));

            auto parsed = juce::JSON::parse (runCli (args));
            asObject (parsed, "locator");
            return parsed;
        }

        static juce::var findNode (const juce::var& node, const std::function<bool (juce::DynamicObject&)>& predicate)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return {};

            if (predicate (*object))
                return node;

            auto children = object->getProperty ("children");

            if (children.isArray())
                for (const auto& child : *children.getArray())
                    if (auto found = findNode (child, predicate); ! found.isVoid())
                        return found;

            return {};
        }

        static juce::var findSnapshotNode (const juce::var& snapshot,
                                           const juce::String& label,
                                           const std::function<bool (juce::DynamicObject&)>& predicate)
        {
            auto tree = asObject (snapshot, "snapshot").getProperty ("tree");
            auto found = findNode (tree, predicate);
            require (! found.isVoid(), "Could not find DemoRunner node: " + label);
            return found;
        }

        static juce::String nodeString (const juce::var& node, const juce::Identifier& property)
        {
            return asObject (node, "node").getProperty (property).toString();
        }

        static juce::String nodeRef (const juce::var& node)
        {
            return nodeString (node, "ref");
        }

        static bool isVisible (juce::DynamicObject& node)
        {
            return (bool) node.getProperty ("visible");
        }

        static bool hasClass (juce::DynamicObject& node, const juce::String& className)
        {
            return node.getProperty ("class").toString().contains (className);
        }

        juce::var visibleNodeByClassAndName (const juce::var& snapshot, const juce::String& className, const juce::String& name)
        {
            return findSnapshotNode (snapshot, className + "=" + name, [&] (juce::DynamicObject& node) {
                return isVisible (node)
                       && hasClass (node, className)
                       && node.getProperty ("name").toString() == name;
            });
        }

        juce::var topLevelTabs (const juce::var& snapshot)
        {
            return findSnapshotNode (snapshot, "DemoContentComponent", [] (juce::DynamicObject& node) {
                return isVisible (node) && hasClass (node, "DemoContentComponent");
            });
        }

        void selectTopLevelTab (const juce::String& tabName)
        {
            auto tabs = topLevelTabs (readSnapshot());
            runCli ({ "-s", sessionName, "select-tab", nodeRef (tabs), "--name", tabName });
            juce::Thread::sleep (250);
        }

        void openDemosPanel()
        {
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Browse Demos", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "list", "--visible", "--timeout-ms", "3000" });
            juce::Thread::sleep (250);
        }

        void clickVisibleListItem (const juce::String& name)
        {
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "3000" });
            juce::Thread::sleep (500);
        }

        void exerciseAccessibilityDemo()
        {
            auto snapshot = readSnapshot();
            auto demoTabs = visibleNodeByClassAndName (snapshot, "juce::TabbedComponent", "Demo tabs");
            auto tabNames = asObject (demoTabs, "Demo tabs").getProperty ("tabNames");
            require (tabNames.isArray() && tabNames.getArray()->size() >= 2, "Accessibility demo tabs did not expose tab names");

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Press me!", "--exact" });

            runCli ({ "-s", sessionName, "check", "--role", "radioButton", "--name", "Button 2", "--exact" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "radioButton", "--name", "Button 2", "--exact", "--value", "true", "--timeout-ms", "3000" });

            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "42" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "42", "--timeout-ms", "3000" });

            demoTabs = visibleNodeByClassAndName (readSnapshot(), "juce::TabbedComponent", "Demo tabs");
            runCli ({ "-s", sessionName, "select-tab", nodeRef (demoTabs), "--name", "Custom Widget" });
            runCli ({ "-s", sessionName, "wait-for-text", "Description", "--timeout-ms", "3000" });
            captureScreenshot ("accessibility-custom-widget.png");

            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--value", "Custom", "--exact", "Automation Custom" });
            require ((int) asObject (readLocator ({ "--role", "editableText", "--value", "Automation Custom", "--exact" }),
                                     "Automation Custom locator").getProperty ("count") >= 1,
                     "Accessibility custom widget title editor did not update");
        }

        void exerciseFlexBoxDemo()
        {
            runCli ({ "-s", sessionName, "check", "--role", "radioButton", "--name", "column", "--exact" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "radioButton", "--name", "column", "--exact", "--value", "true", "--timeout-ms", "3000" });

            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--value", "1", "--exact", "--nth", "0", "2" });
            require ((int) asObject (readLocator ({ "--role", "editableText", "--value", "2", "--exact" }),
                                     "FlexBox editor locator").getProperty ("count") >= 1,
                     "FlexBox flex-grow editor did not update");

            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--value", "stretch", "--exact", "--nth", "0", "--text", "center" });
            require ((int) asObject (readLocator ({ "--role", "comboBox", "--value", "center", "--exact" }),
                                     "FlexBox combo locator").getProperty ("count") >= 1,
                     "FlexBox align-self combo did not update");
        }

        void exerciseSettings()
        {
            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--value", "LookAndFeel_V4 (Dark)", "--exact", "--text", "LookAndFeel_V4 (Light)" });
            require ((int) asObject (readLocator ({ "--role", "comboBox", "--value", "LookAndFeel_V4 (Light)", "--exact" }),
                                     "Settings LookAndFeel locator").getProperty ("count") >= 1,
                     "Settings LookAndFeel combo did not update");
            captureScreenshot ("settings-light.png");
        }

        void captureScreenshot (const juce::String& name)
        {
            runCli ({ "-s", sessionName, "wait", "--ms", "250" });
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
            require (lines.size() >= 4, "MCP E2E expected at least 4 response lines, got " + juce::String (lines.size()));

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
        DemoRunnerE2E().run();
        std::cout << "ok - DemoRunner automation e2e passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "DemoRunner automation e2e failed: " << e.what() << "\n";
        return 1;
    }
}
