#include <melatonin_inspector/melatonin_inspector.h>

#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
    #include <iostream>
    #include <stdexcept>
#endif

namespace
{
#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
    constexpr auto sessionName = "automation_fixture";

    void require (bool condition, const juce::String& message)
    {
        if (!condition)
            throw std::runtime_error (message.toStdString());
    }

    juce::File tempDirectory()
    {
        const auto temp = juce::SystemStats::getEnvironmentVariable (
        #if JUCE_WINDOWS
            "TEMP",
        #else
            "TMPDIR",
        #endif
            {});

        return temp.isNotEmpty() ? juce::File (temp) : juce::File ("/tmp");
    }

    juce::File sessionsDirectory()
    {
        return tempDirectory().getChildFile ("melatonin_inspector").getChildFile ("sessions");
    }

    void cleanupSessionFiles()
    {
        auto directory = sessionsDirectory();

        if (!directory.isDirectory())
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (directory, false, "*.json", juce::File::findFiles))
        {
            auto file = entry.getFile();
            auto parsed = juce::JSON::parse (file.loadFileAsString());

            if (auto* object = parsed.getDynamicObject())
                if (object->getProperty ("session").toString() == sessionName)
                    file.deleteFile();
        }
    }

    juce::File inferBuildDirectory()
    {
        const auto overridePath = juce::SystemStats::getEnvironmentVariable ("MELATONIN_AUTOMATION_BUILD_DIR", {});

        if (overridePath.isNotEmpty())
            return juce::File (overridePath);

        auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    #if JUCE_MAC
        return executable.getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory();
    #elif JUCE_WINDOWS
        return executable.getParentDirectory().getParentDirectory().getParentDirectory();
    #else
        return executable.getParentDirectory().getParentDirectory();
    #endif
    }

    juce::File findMelatoninUi()
    {
        const auto overridePath = juce::SystemStats::getEnvironmentVariable ("MELATONIN_UI", {});

        if (overridePath.isNotEmpty())
            return juce::File (overridePath);

        auto artefacts = inferBuildDirectory().getChildFile ("melatonin-ui_artefacts");

    #if JUCE_WINDOWS
        const auto executableName = "melatonin-ui.exe";

        for (auto configuration : { "Debug", "Release", "RelWithDebInfo", "MinSizeRel" })
        {
            auto candidate = artefacts.getChildFile (configuration).getChildFile (executableName);

            if (candidate.existsAsFile())
                return candidate;
        }

        return artefacts.getChildFile (executableName);
    #else
        return artefacts.getChildFile ("melatonin-ui");
    #endif
    }

    juce::StringArray makeArgs (std::initializer_list<juce::String> values)
    {
        juce::StringArray result;

        for (const auto& value : values)
            result.add (value);

        return result;
    }

    juce::String shellQuote (juce::String value)
    {
    #if JUCE_WINDOWS
        return "\"" + value.replace ("\"", "\\\"") + "\"";
    #else
        return "'" + value.replace ("'", "'\"'\"'") + "'";
    #endif
    }

    juce::DynamicObject& asObject (const juce::var& value, const juce::String& context)
    {
        auto* object = value.getDynamicObject();
        require (object != nullptr, context + " is not a JSON object");
        return *object;
    }

    juce::var findNode (const juce::var& node, const std::function<bool (juce::DynamicObject&)>& predicate)
    {
        auto* object = node.getDynamicObject();

        if (object == nullptr)
            return {};

        if (predicate (*object))
            return node;

        auto children = object->getProperty ("children");

        if (children.isArray())
            for (const auto& child : *children.getArray())
                if (auto found = findNode (child, predicate); !found.isVoid())
                    return found;

        return {};
    }

    juce::var findByComponentName (const juce::var& snapshot, const juce::String& componentName)
    {
        auto tree = asObject (snapshot, "snapshot").getProperty ("tree");
        return findNode (tree, [&componentName] (juce::DynamicObject& node) {
            return node.getProperty ("componentName").toString() == componentName;
        });
    }

    juce::String refByComponentName (const juce::var& snapshot, const juce::String& componentName)
    {
        auto node = findByComponentName (snapshot, componentName);
        require (!node.isVoid(), "Could not find componentName " + componentName);
        return asObject (node, componentName).getProperty ("ref").toString();
    }

    juce::Rectangle<int> boundsOf (const juce::var& node)
    {
        auto bounds = asObject (node, "node").getProperty ("bounds");
        auto& boundsObject = asObject (bounds, "bounds");

        return { (int) boundsObject.getProperty ("x"),
                 (int) boundsObject.getProperty ("y"),
                 (int) boundsObject.getProperty ("w"),
                 (int) boundsObject.getProperty ("h") };
    }

    double valueOf (const juce::var& node)
    {
        return asObject (node, "node").getProperty ("value").toString().getDoubleValue();
    }

    void assertStatus (const juce::var& snapshot, const juce::String& expected)
    {
        auto status = findByComponentName (snapshot, "fixture.status");
        require (!status.isVoid(), "snapshot is missing fixture.status");

        auto& object = asObject (status, "fixture.status");
        auto actual = object.getProperty ("name").toString()
                      + "\n" + object.getProperty ("title").toString()
                      + "\n" + object.getProperty ("value").toString();

        require (actual.contains (expected), "expected status \"" + expected + "\", got \"" + actual + "\"");
    }

    int readBigEndianInt (const juce::MemoryBlock& bytes, size_t offset)
    {
        auto* data = static_cast<const unsigned char*> (bytes.getData());
        return ((int) data[offset] << 24) | ((int) data[offset + 1] << 16) | ((int) data[offset + 2] << 8) | (int) data[offset + 3];
    }

    juce::MemoryBlock loadPng (const juce::File& file, const juce::String& label)
    {
        juce::MemoryBlock bytes;
        require (file.loadFileAsData (bytes), label + " could not be read: " + file.getFullPathName());

        static constexpr unsigned char pngSignature[] { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
        require (bytes.getSize() > 1000, label + " is unexpectedly small: " + juce::String ((int) bytes.getSize()) + " bytes");
        require (bytes.getSize() >= sizeof (pngSignature), label + " is not a PNG: " + file.getFullPathName());

        auto* data = static_cast<const unsigned char*> (bytes.getData());

        for (size_t i = 0; i < sizeof (pngSignature); ++i)
            require (data[i] == pngSignature[i], label + " is not a PNG: " + file.getFullPathName());

        return bytes;
    }

    void assertPng (const juce::File& file, const juce::String& label)
    {
        loadPng (file, label);
    }

    void assertPngSize (const juce::File& file, int width, int height, const juce::String& label)
    {
        auto bytes = loadPng (file, label);
        auto actualWidth = readBigEndianInt (bytes, 16);
        auto actualHeight = readBigEndianInt (bytes, 20);

        require (actualWidth == width && actualHeight == height,
                 label + " expected " + juce::String (width) + "x" + juce::String (height)
                     + ", got " + juce::String (actualWidth) + "x" + juce::String (actualHeight));
    }

    class AutomationFixtureSelfTest : public juce::Thread
    {
    public:
        explicit AutomationFixtureSelfTest (std::function<void (int)> onCompleteCallback)
            : juce::Thread ("Automation Fixture Self Test"),
              onComplete (std::move (onCompleteCallback)),
              cliPath (findMelatoninUi())
        {
        }

        ~AutomationFixtureSelfTest() override
        {
            signalThreadShouldExit();
            stopThread (2000);
        }

        void run() override
        {
            try
            {
                runChecks();
                std::cout << "ok - CLI/MCP automation e2e passed\n";
                std::cout << "root screenshot: " << rootScreenshot.getFullPathName() << "\n";
                finish (0);
            }
            catch (const std::exception& e)
            {
                std::cerr << "automation_fixture self-test failed: " << e.what() << "\n";
                finish (1);
            }
        }

    private:
        std::function<void (int)> onComplete;
        juce::File cliPath;
        juce::File screenshotDirectory = juce::SystemStats::getEnvironmentVariable ("MELATONIN_SCREENSHOT_DIR", {}).isNotEmpty()
                                             ? juce::File (juce::SystemStats::getEnvironmentVariable ("MELATONIN_SCREENSHOT_DIR", {}))
                                             : tempDirectory();
        juce::File rootScreenshot;

        void finish (int returnCode)
        {
            auto callback = onComplete;
            juce::MessageManager::callAsync ([callback, returnCode] {
                if (callback)
                    callback (returnCode);
            });
        }

        void runChecks()
        {
            require (cliPath.existsAsFile(), "Missing melatonin-ui CLI: " + cliPath.getFullPathName());
            waitForSession();

            auto listOutput = runCli ({ "list" });
            require (listOutput.contains (juce::String (sessionName) + " "), "CLI list did not show the automation_fixture process");

            runMcpSmokeCheck();

            auto snapshot = readSnapshot();
            auto capabilities = juce::JSON::parse (runCli ({ "-s", sessionName, "capabilities" }));
            auto& capabilitiesObject = asObject (capabilities, "capabilities");
            require ((int) capabilitiesObject.getProperty ("protocolVersion") == 1, "capabilities returned the wrong protocol version");

            auto securityValue = capabilitiesObject.getProperty ("security");
            auto& security = asObject (securityValue, "capabilities.security");
            require ((bool) security.getProperty ("allowInput"), "capabilities did not expose allowInput=true");
            require ((bool) security.getProperty ("allowMutation"), "capabilities did not expose allowMutation=true");
            require ((bool) security.getProperty ("allowFileWrite"), "capabilities did not expose allowFileWrite=true");
            require (security.getProperty ("artifactRoot").toString() == screenshotDirectory.getFullPathName(),
                     "capabilities returned the wrong artifact root");

            auto snapshotAgain = readSnapshot();
            auto& snapshotObject = asObject (snapshot, "snapshot");
            auto& snapshotAgainObject = asObject (snapshotAgain, "snapshotAgain");
            auto initialStateHash = snapshotObject.getProperty ("stateHash").toString();
            require (initialStateHash.isNotEmpty(), "snapshot is missing stateHash");
            require (initialStateHash == snapshotAgainObject.getProperty ("stateHash").toString(),
                     "repeated snapshots without UI changes should keep the same stateHash");
            require ((int) snapshotObject.getProperty ("generation") != (int) snapshotAgainObject.getProperty ("generation"),
                     "repeated snapshots should still advance the ref generation");
            snapshot = snapshotAgain;

            require (!findByComponentName (snapshot, "fixture.tabs").isVoid(), "snapshot is missing top-level tabs");
            require (!findByComponentName (snapshot, "controls.slider").isVoid(), "snapshot is missing the controls slider");

            auto deniedScreenshot = screenshotDirectory.getSiblingFile ("melatonin-automation-denied.png");
            auto deniedOutput = runCliExpectFailure ({ "-s", sessionName, "screenshot", "--target", "root", "--file", deniedScreenshot.getFullPathName() });
            require (deniedOutput.contains ("artifact root") || deniedOutput.contains ("artifact_path_denied"),
                     "screenshot outside artifact root should fail with artifact_path_denied\n" + deniedOutput);

            rootScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-root.png");
            runCli ({ "-s", sessionName, "screenshot", "--target", "root", "--file", rootScreenshot.getFullPathName() });
            assertPng (rootScreenshot, "root screenshot");

            auto buttonScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-button.png");
            auto editorButton = findByComponentName (snapshot, "nav.editor");
            auto editorButtonBounds = boundsOf (editorButton);
            runCli ({ "-s", sessionName, "screenshot", "--ref", asObject (editorButton, "nav.editor").getProperty ("ref").toString(), "--file", buttonScreenshot.getFullPathName() });
            assertPng (buttonScreenshot, "button screenshot");
            assertPngSize (buttonScreenshot, editorButtonBounds.getWidth(), editorButtonBounds.getHeight(), "button screenshot");

            clickXYAtNode (findByComponentName (snapshot, "controls.power"));
            snapshot = readSnapshot();
            require (asObject (snapshot, "snapshot after click").getProperty ("stateHash").toString() != initialStateHash,
                     "clicking the power button should change the semantic stateHash");
            require ((bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("toggleState"),
                     "click-xy did not toggle the power button");
            assertStatus (snapshot, "Status: Power On");

            auto sliderBefore = valueOf (findByComponentName (snapshot, "controls.slider"));
            dragRef (refByComponentName (snapshot, "controls.slider"), 90, 0);
            snapshot = readSnapshot();
            auto sliderAfter = valueOf (findByComponentName (snapshot, "controls.slider"));
            require (sliderAfter > sliderBefore,
                     "slider drag did not increase value: " + juce::String (sliderBefore) + " -> " + juce::String (sliderAfter));
            assertStatus (snapshot, "Status: Slider");

            clickRef (refByComponentName (snapshot, "nav.editor"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Editor");
            require (!findByComponentName (snapshot, "editor.text").isVoid(), "editor page did not expose its text editor");

            auto editorRef = refByComponentName (snapshot, "editor.text");
            typeRef (editorRef, "hello from automation");
            snapshot = readSnapshot();
            pressRef (refByComponentName (snapshot, "editor.text"), "!");
            snapshot = readSnapshot();
            clickRef (refByComponentName (snapshot, "editor.apply"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Applied hello from automation!");

            pressRef (refByComponentName (snapshot, "editor.text"), "backspace");
            snapshot = readSnapshot();
            clickRef (refByComponentName (snapshot, "editor.apply"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Applied hello from automation");

            clickRef (refByComponentName (snapshot, "nav.advanced"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Advanced");
            require (!findByComponentName (snapshot, "advanced.tabs").isVoid(), "advanced page did not expose nested tabs");

            clickRef (refByComponentName (snapshot, "advanced.goActions"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Nested Actions");
            require (!findByComponentName (snapshot, "advanced.reset").isVoid(), "nested Actions tab did not expose Reset All");
            require (!findByComponentName (snapshot, "advanced.dragBox").isVoid(), "nested Actions tab did not expose Drag Box");

            auto dragBoxBefore = findByComponentName (snapshot, "advanced.dragBox");
            auto dragBoxBeforeBounds = boundsOf (dragBoxBefore);
            dragRef (asObject (dragBoxBefore, "advanced.dragBox").getProperty ("ref").toString(), 40, 15);
            snapshot = readSnapshot();
            auto dragBoxAfterBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));
            require (dragBoxAfterBounds.getX() == dragBoxBeforeBounds.getX() + 40, "drag did not move Drag Box on the x axis");
            require (dragBoxAfterBounds.getY() == dragBoxBeforeBounds.getY() + 15, "drag did not move Drag Box on the y axis");
            assertStatus (snapshot, "Status: DragBox");

            auto resetRef = refByComponentName (snapshot, "advanced.reset");
            runCli ({ "-s", sessionName, "set-bounds", resetRef, "--x", "20", "--y", "24", "--w", "180", "--h", "34" });
            snapshot = readSnapshot();
            auto resetAfterBounds = findByComponentName (snapshot, "advanced.reset");
            auto resetBounds = boundsOf (resetAfterBounds);
            require (resetBounds.getWidth() == 180 && resetBounds.getHeight() == 34, "set-bounds did not update Reset All dimensions");

            runCli ({ "-s", sessionName, "set-property", asObject (resetAfterBounds, "advanced.reset").getProperty ("ref").toString(), "alpha", "0.9" });
            snapshot = readSnapshot();
            clickRef (refByComponentName (snapshot, "advanced.reset"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Reset");
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

            return runProcess (command, "melatonin-ui " + args.joinIntoString (" "), true);
        }

        juce::String runCliExpectFailure (std::initializer_list<juce::String> args)
        {
            juce::StringArray command;
            command.add (cliPath.getFullPathName());
            command.addArray (makeArgs (args));

            return runProcess (command, "melatonin-ui " + makeArgs (args).joinIntoString (" "), false);
        }

        juce::String runProcess (const juce::StringArray& command, const juce::String& label, bool expectSuccess)
        {
            auto displayCommand = command.joinIntoString (" ");

            juce::ChildProcess process;
            require (process.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr),
                     "Could not start " + displayCommand);

            juce::String output;
            auto deadline = juce::Time::currentTimeMillis() + 10000;
            char buffer[4096] {};

            while (process.isRunning())
            {
                if (auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer)); bytesRead > 0)
                    output << juce::String::fromUTF8 (buffer, bytesRead);

                if (juce::Time::currentTimeMillis() > deadline || threadShouldExit())
                {
                    process.kill();
                    throw std::runtime_error (("Timed out running " + label + "\n" + output).toStdString());
                }

                juce::Thread::sleep (10);
            }

            for (;;)
            {
                auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer));

                if (bytesRead <= 0)
                    break;

                output << juce::String::fromUTF8 (buffer, bytesRead);
            }

            if (expectSuccess)
            {
                require (process.getExitCode() == 0,
                         label + " failed with exit code " + juce::String ((int) process.getExitCode()) + "\n" + output);
            }
            else
            {
                require (process.getExitCode() != 0,
                         label + " unexpectedly succeeded\n" + output);
            }

            return output;
        }

        juce::String runMcpBatch (std::initializer_list<juce::String> requests)
        {
            auto requestFile = tempDirectory().getNonexistentChildFile ("melatonin-mcp-requests", ".jsonl");
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

            auto output = runProcess (command, "melatonin-ui mcp", true);
            requestFile.deleteFile();
            return output;
        }

        juce::var parseMcpLine (const juce::StringArray& lines, int index)
        {
            require (juce::isPositiveAndBelow (index, lines.size()), "MCP response " + juce::String (index) + " was not written");

            auto parsed = juce::JSON::parse (lines[index]);
            asObject (parsed, "MCP response " + juce::String (index));
            return parsed;
        }

        juce::var assertMcpResult (const juce::var& response, int expectedId)
        {
            auto& responseObject = asObject (response, "MCP response");
            require ((int) responseObject.getProperty ("id") == expectedId,
                     "MCP response id mismatch: expected " + juce::String (expectedId)
                         + ", got " + responseObject.getProperty ("id").toString());
            require (responseObject.getProperty ("error").isVoid(), "MCP response returned an error: " + juce::JSON::toString (response, true));

            auto result = responseObject.getProperty ("result");
            asObject (result, "MCP result");
            return result;
        }

        void runMcpSmokeCheck()
        {
            auto output = runMcpBatch ({
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})",
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"juce_capabilities","arguments":{"session":"automation_fixture"}}})",
                R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"juce_snapshot","arguments":{"session":"automation_fixture","format":"text","depth":12}}})"
            });

            auto lines = juce::StringArray::fromLines (output);
            require (lines.size() >= 4, "MCP smoke expected at least 4 response lines, got " + juce::String (lines.size()) + "\n" + output);

            auto initializeResult = assertMcpResult (parseMcpLine (lines, 0), 1);
            auto& initialize = asObject (initializeResult, "MCP initialize result");
            auto serverInfoValue = initialize.getProperty ("serverInfo");
            auto& serverInfo = asObject (serverInfoValue, "MCP serverInfo");
            require (serverInfo.getProperty ("name").toString() == "melatonin-mcp", "MCP initialize returned the wrong server name");

            auto toolsListResult = assertMcpResult (parseMcpLine (lines, 1), 2);
            auto& toolsList = asObject (toolsListResult, "MCP tools/list result");
            auto tools = toolsList.getProperty ("tools");
            require (tools.isArray(), "MCP tools/list did not return a tools array");

            bool foundSnapshotTool = false;

            for (const auto& toolInfo : *tools.getArray())
                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_snapshot")
                    foundSnapshotTool = true;

            require (foundSnapshotTool, "MCP tools/list did not expose juce_snapshot");

            auto capabilitiesCallResult = assertMcpResult (parseMcpLine (lines, 2), 3);
            auto& capabilitiesCall = asObject (capabilitiesCallResult, "MCP capabilities result");
            auto capabilitiesContent = capabilitiesCall.getProperty ("content");
            require (capabilitiesContent.isArray() && !capabilitiesContent.getArray()->isEmpty(), "MCP capabilities did not return content");
            require (asObject (capabilitiesContent.getArray()->getReference (0), "MCP capabilities content").getProperty ("text").toString().contains ("protocolVersion"),
                     "MCP capabilities content did not include protocolVersion");

            auto snapshotCallResult = assertMcpResult (parseMcpLine (lines, 3), 4);
            auto& snapshotCall = asObject (snapshotCallResult, "MCP snapshot result");
            auto content = snapshotCall.getProperty ("content");
            require (content.isArray() && !content.getArray()->isEmpty(), "MCP snapshot did not return content");

            auto text = asObject (content.getArray()->getReference (0), "MCP snapshot content").getProperty ("text").toString();
            require (text.contains ("fixture.tabs"), "MCP snapshot content did not include the fixture tree");
        }

        juce::var readSnapshot()
        {
            auto parsed = juce::JSON::parse (runCli ({ "-s", sessionName, "snapshot", "--format", "json", "--depth", "12" }));
            asObject (parsed, "snapshot");
            return parsed;
        }

        void waitForSession()
        {
            auto deadline = juce::Time::currentTimeMillis() + 15000;

            while (juce::Time::currentTimeMillis() < deadline && !threadShouldExit())
            {
                auto listOutput = runCli ({ "list" });

                if (listOutput.contains (juce::String (sessionName) + " "))
                    return;

                juce::Thread::sleep (250);
            }

            throw std::runtime_error ("Timed out waiting for automation_fixture to advertise an automation session");
        }

        void clickRef (const juce::String& ref)
        {
            runCli ({ "-s", sessionName, "click", ref });
        }

        void clickXYAtNode (const juce::var& node)
        {
            require (!node.isVoid(), "click-xy target node is missing");
            auto bounds = boundsOf (node);
            runCli ({ "-s",
                      sessionName,
                      "click-xy",
                      juce::String (bounds.getCentreX()),
                      juce::String (bounds.getCentreY()) });
        }

        void typeRef (const juce::String& ref, const juce::String& text)
        {
            runCli ({ "-s", sessionName, "type", ref, text });
        }

        void pressRef (const juce::String& ref, const juce::String& key)
        {
            runCli ({ "-s", sessionName, "press", key, "--ref", ref });
        }

        void dragRef (const juce::String& ref, int dx, int dy)
        {
            runCli ({ "-s", sessionName, "drag", ref, "--dx", juce::String (dx), "--dy", juce::String (dy) });
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationFixtureSelfTest)
    };
#endif

    class CallbackTabbedComponent : public juce::TabbedComponent
    {
    public:
        explicit CallbackTabbedComponent (juce::TabbedButtonBar::Orientation orientation)
            : juce::TabbedComponent (orientation)
        {
        }

        void currentTabChanged (int index, const juce::String& name) override
        {
            if (onCurrentTabChanged)
                onCurrentTabChanged (index, name);
        }

        std::function<void (int, const juce::String&)> onCurrentTabChanged;
    };

    class ControlsPage : public juce::Component
    {
    public:
        ControlsPage()
        {
            setName ("Controls Page");

            title.setText ("Controls Page", juce::dontSendNotification);
            title.setName ("controls.title");
            addAndMakeVisible (title);

            goEditor.setButtonText ("Go Editor");
            goEditor.setName ("nav.editor");
            addAndMakeVisible (goEditor);

            toggle.setButtonText ("Power Toggle");
            toggle.setName ("controls.power");
            addAndMakeVisible (toggle);

            slider.setName ("controls.slider");
            slider.setRange (0.0, 100.0, 1.0);
            slider.setValue (25.0);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 24);
            addAndMakeVisible (slider);

            combo.setName ("controls.combo");
            combo.addItem ("Alpha", 1);
            combo.addItem ("Beta", 2);
            combo.addItem ("Gamma", 3);
            combo.setSelectedId (1);
            addAndMakeVisible (combo);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16);
            title.setBounds (area.removeFromTop (28));
            goEditor.setBounds (area.removeFromTop (34).removeFromLeft (140));
            area.removeFromTop (10);
            toggle.setBounds (area.removeFromTop (30).removeFromLeft (180));
            area.removeFromTop (10);
            slider.setBounds (area.removeFromTop (36).removeFromLeft (360));
            area.removeFromTop (10);
            combo.setBounds (area.removeFromTop (30).removeFromLeft (180));
        }

        juce::TextButton goEditor;
        juce::ToggleButton toggle;
        juce::Slider slider;
        juce::ComboBox combo;

    private:
        juce::Label title;
    };

    class DragBox : public juce::Component
    {
    public:
        DragBox()
        {
            setName ("advanced.dragBox");
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::darkslategrey);
            g.setColour (juce::Colours::white);
            g.drawFittedText ("Drag Box", getLocalBounds(), juce::Justification::centred, 1);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            dragStartBounds = getBounds();
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            setBounds (dragStartBounds.translated (event.getDistanceFromDragStartX(), event.getDistanceFromDragStartY()));

            if (onDragged)
                onDragged (getBounds());
        }

        std::function<void (juce::Rectangle<int>)> onDragged;

    private:
        juce::Rectangle<int> dragStartBounds;
    };

    class EditorPage : public juce::Component
    {
    public:
        EditorPage()
        {
            setName ("Editor Page");

            title.setText ("Editor Page", juce::dontSendNotification);
            title.setName ("editor.title");
            addAndMakeVisible (title);

            text.setName ("editor.text");
            text.setTextToShowWhenEmpty ("Type here", juce::Colours::grey);
            addAndMakeVisible (text);

            apply.setButtonText ("Apply Text");
            apply.setName ("editor.apply");
            addAndMakeVisible (apply);

            goAdvanced.setButtonText ("Go Advanced");
            goAdvanced.setName ("nav.advanced");
            addAndMakeVisible (goAdvanced);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16);
            title.setBounds (area.removeFromTop (28));
            text.setBounds (area.removeFromTop (36).removeFromLeft (300));
            area.removeFromTop (10);
            apply.setBounds (area.removeFromTop (34).removeFromLeft (140));
            area.removeFromTop (10);
            goAdvanced.setBounds (area.removeFromTop (34).removeFromLeft (140));
        }

        juce::TextEditor text;
        juce::TextButton apply;
        juce::TextButton goAdvanced;

    private:
        juce::Label title;
    };

    class AdvancedPage : public juce::Component
    {
    public:
        AdvancedPage()
        {
            setName ("Advanced Page");

            nestedTabs.setName ("advanced.tabs");
            nestedTabs.addTab ("Metrics", juce::Colours::darkgrey, &metrics, false);
            nestedTabs.addTab ("Actions", juce::Colours::darkgrey, &actions, false);
            addAndMakeVisible (nestedTabs);

            metrics.setName ("Metrics Page");
            metricLabel.setText ("Metrics Ready", juce::dontSendNotification);
            metricLabel.setName ("advanced.metrics.label");
            metrics.addAndMakeVisible (metricLabel);

            goActions.setButtonText ("Go Actions");
            goActions.setName ("advanced.goActions");
            metrics.addAndMakeVisible (goActions);

            actions.setName ("Actions Page");
            reset.setButtonText ("Reset All");
            reset.setName ("advanced.reset");
            actions.addAndMakeVisible (reset);

            actions.addAndMakeVisible (dragBox);

            goActions.onClick = [this] {
                nestedTabs.setCurrentTabIndex (1);
            };

            nestedTabs.onCurrentTabChanged = [this] (int index, const juce::String& name) {
                juce::ignoreUnused (index);
                if (onNestedTabChanged)
                    onNestedTabChanged (name);
            };
        }

        void resized() override
        {
            nestedTabs.setBounds (getLocalBounds().reduced (16));
            auto metricsArea = metrics.getLocalBounds().reduced (16);
            metricLabel.setBounds (metricsArea.removeFromTop (30));
            metricsArea.removeFromTop (10);
            goActions.setBounds (metricsArea.removeFromTop (34).removeFromLeft (140));

            reset.setBounds (actions.getLocalBounds().reduced (16).removeFromTop (34).removeFromLeft (140));
            dragBox.setBounds (240, 24, 100, 42);
        }

        CallbackTabbedComponent nestedTabs { juce::TabbedButtonBar::TabsAtTop };
        juce::TextButton goActions;
        juce::TextButton reset;
        DragBox dragBox;
        std::function<void (const juce::String&)> onNestedTabChanged;

    private:
        juce::Component metrics;
        juce::Component actions;
        juce::Label metricLabel;
    };

    class AutomationRoot : public juce::Component
    {
    public:
        AutomationRoot()
        {
            setName ("Automation Fixture Root");

            status.setName ("fixture.status");
            status.setText ("Status: Controls", juce::dontSendNotification);
            addAndMakeVisible (status);

            tabs.setName ("fixture.tabs");
            tabs.addTab ("Controls", juce::Colours::lightgrey, &controls, false);
            tabs.addTab ("Editor", juce::Colours::lightgrey, &editor, false);
            tabs.addTab ("Advanced", juce::Colours::lightgrey, &advanced, false);
            addAndMakeVisible (tabs);

            controls.goEditor.onClick = [this] {
                tabs.setCurrentTabIndex (1);
                setStatus ("Status: Editor");
            };

            controls.toggle.onClick = [this] {
                setStatus (controls.toggle.getToggleState() ? "Status: Power On" : "Status: Power Off");
            };

            controls.slider.onValueChange = [this] {
                setStatus ("Status: Slider " + juce::String (juce::roundToInt (controls.slider.getValue())));
            };

            editor.apply.onClick = [this] {
                setStatus ("Status: Applied " + editor.text.getText());
            };

            editor.goAdvanced.onClick = [this] {
                tabs.setCurrentTabIndex (2);
                setStatus ("Status: Advanced");
            };

            advanced.reset.onClick = [this] {
                editor.text.clear();
                controls.toggle.setToggleState (false, juce::dontSendNotification);
                controls.slider.setValue (25.0, juce::dontSendNotification);
                setStatus ("Status: Reset");
            };

            advanced.onNestedTabChanged = [this] (const juce::String& name) {
                setStatus ("Status: Nested " + name);
            };

            advanced.dragBox.onDragged = [this] (juce::Rectangle<int> bounds) {
                setStatus ("Status: DragBox " + juce::String (bounds.getX()) + "," + juce::String (bounds.getY()));
            };

            tabs.onCurrentTabChanged = [this] (int index, const juce::String& name) {
                juce::ignoreUnused (index);
                setStatus ("Status: " + name);
            };
        }

        void resized() override
        {
            auto area = getLocalBounds();
            status.setBounds (area.removeFromBottom (34).reduced (12, 4));
            tabs.setBounds (area);
        }

    private:
        void setStatus (const juce::String& text)
        {
            status.setText (text, juce::dontSendNotification);
        }

        juce::Label status;
        CallbackTabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
        ControlsPage controls;
        EditorPage editor;
        AdvancedPage advanced;
    };
}

class AutomationFixtureApp : public juce::JUCEApplication
{
public:
    void initialise (const juce::String&) override
    {
#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
        cleanupSessionFiles();
#endif

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        inspector = std::make_unique<melatonin::Inspector> (mainWindow->content);

#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
        melatonin::AutomationOptions options;
        options.sessionName = sessionName;
        options.allowFileWrite = true;
        options.artifactRoot = juce::SystemStats::getEnvironmentVariable ("MELATONIN_SCREENSHOT_DIR", {}).isNotEmpty()
                                   ? juce::File (juce::SystemStats::getEnvironmentVariable ("MELATONIN_SCREENSHOT_DIR", {}))
                                   : tempDirectory();
        inspector->enableAutomation (options);
#endif

        inspector->setVisible (true);
        juce::Process::makeForegroundProcess();
        mainWindow->toFront (true);

#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
        selfTest = std::make_unique<AutomationFixtureSelfTest> ([this] (int returnCode) {
            setApplicationReturnValue (returnCode);
            quit();
        });
        selfTest->startThread();
#endif
    }

    void shutdown() override
    {
#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
        selfTest = nullptr;
#endif
        inspector = nullptr;
        mainWindow = nullptr;
    }

    const juce::String getApplicationName() override { return "automation_fixture"; }
    const juce::String getApplicationVersion() override { return "v1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name, juce::Colours::lightgrey, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentNonOwned (&content, true);
            centreWithSize (640, 420);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        AutomationRoot content;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<melatonin::Inspector> inspector;
#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
    std::unique_ptr<AutomationFixtureSelfTest> selfTest;
#endif
};

START_JUCE_APPLICATION (AutomationFixtureApp)
