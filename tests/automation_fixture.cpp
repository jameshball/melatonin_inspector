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
        require (bytes.getSize() > sizeof (pngSignature),
                 label + " is unexpectedly small: " + juce::String ((int) bytes.getSize()) + " bytes");
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

            auto windows = juce::JSON::parse (runCli ({ "-s", sessionName, "windows" }));
            auto windowsArray = asObject (windows, "windows").getProperty ("windows");
            require (windowsArray.isArray() && windowsArray.getArray()->size() == 1, "windows should expose the owned root window");
            require (asObject (windowsArray.getArray()->getReference (0), "root window").getProperty ("id").toString() == "root",
                     "root window id should be root");

            auto traceFile = screenshotDirectory.getChildFile ("melatonin-automation-trace.json");
            runCli ({ "-s", sessionName, "trace-start", "--file", traceFile.getFullPathName() });
            runCli ({ "-s", sessionName, "snapshot", "--format", "json", "--depth", "4" });
            auto tracedFailure = runCliExpectFailure ({ "-s", sessionName, "click", "--component-id", "trace.missing", "--timeout-ms", "50" });
            require (tracedFailure.contains ("Timed out") || tracedFailure.contains ("Locator did not match"),
                     "expected traced missing locator click to fail\n" + tracedFailure);
            auto traceStop = juce::JSON::parse (runCli ({ "-s", sessionName, "trace-stop" }));
            require ((int) asObject (traceStop, "trace-stop").getProperty ("events") >= 2, "trace-stop did not record events");
            auto traceJson = juce::JSON::parse (traceFile.loadFileAsString());
            auto traceEvents = asObject (traceJson, "trace file").getProperty ("events");
            require (traceEvents.isArray() && !traceEvents.getArray()->isEmpty(), "trace file did not contain events");
            bool foundFailedTraceEvent = false;
            bool foundStructuredTraceEvent = false;

            for (const auto& traceEvent : *traceEvents.getArray())
            {
                auto& eventObject = asObject (traceEvent, "trace event");
                auto params = eventObject.getProperty ("params");
                auto result = eventObject.getProperty ("result");
                auto* resultObject = result.getDynamicObject();

                if (resultObject != nullptr && resultObject->getProperty ("text").isString())
                    require (resultObject->getProperty ("text").toString().length() <= 515,
                             "trace result text summary should be capped");

                foundStructuredTraceEvent = foundStructuredTraceEvent
                                            || ((double) eventObject.getProperty ("elapsedMs") >= 0.0
                                                && params.isObject()
                                                && result.isObject());
                foundFailedTraceEvent = foundFailedTraceEvent
                                        || (!(bool) eventObject.getProperty ("ok")
                                            && eventObject.getProperty ("error").toString().isNotEmpty()
                                            && eventObject.getProperty ("message").toString().isNotEmpty());
            }

            require (foundStructuredTraceEvent, "trace file did not include structured params/result/timing fields");
            require (foundFailedTraceEvent, "trace file did not include the expected failed action event");

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

            auto& topTabs = asObject (findByComponentName (snapshot, "fixture.tabs"), "fixture.tabs");
            auto topTabNames = topTabs.getProperty ("tabNames");
            require (topTabNames.isArray() && topTabNames.getArray()->size() == 3, "snapshot did not expose fixture tab names");
            require (topTabs.getProperty ("currentTab").toString() == "Controls", "snapshot did not expose current fixture tab");

            auto& sliderMetadata = asObject (findByComponentName (snapshot, "controls.slider"), "controls.slider");
            require ((double) sliderMetadata.getProperty ("minimum") == 0.0, "slider metadata did not expose minimum");
            require ((double) sliderMetadata.getProperty ("maximum") == 100.0, "slider metadata did not expose maximum");
            require ((double) sliderMetadata.getProperty ("interval") == 1.0, "slider metadata did not expose interval");

            auto& comboMetadata = asObject (findByComponentName (snapshot, "controls.combo"), "controls.combo");
            auto comboOptions = comboMetadata.getProperty ("options");
            require (comboOptions.isArray() && comboOptions.getArray()->size() == 3, "combo metadata did not expose all options");
            require (comboMetadata.getProperty ("selectedId").toString() == "1", "combo metadata did not expose selected id");
            require (comboMetadata.getProperty ("selectedText").toString() == "Alpha", "combo metadata did not expose selected text");

            auto& listMetadata = asObject (findByComponentName (snapshot, "controls.optionList"), "controls.optionList");
            auto listOptions = listMetadata.getProperty ("options");
            require ((int) listMetadata.getProperty ("rowCount") == 3, "ListBox metadata did not expose row count");
            require (listOptions.isArray() && listOptions.getArray()->size() == 3, "ListBox metadata did not expose row options");

            auto& editorMetadata = asObject (findByComponentName (snapshot, "editor.text"), "editor.text");
            require ((bool) editorMetadata.getProperty ("editable"), "TextEditor metadata did not expose editable=true");
            require (! (bool) editorMetadata.getProperty ("readOnly"), "TextEditor metadata did not expose readOnly=false");

            auto buttonLocator = readLocator ({ "--role", "button", "--name", "Go Editor" });
            require ((int) asObject (buttonLocator, "button locator").getProperty ("count") == 1,
                     "role/name locator did not find Go Editor");

            auto sliderLocator = readLocator ({ "--test-id", "controls.slider" });
            require ((int) asObject (sliderLocator, "slider locator").getProperty ("count") == 1,
                     "test id locator did not find controls.slider");

            auto valueLocator = readLocator ({ "--value", "25" });
            require ((int) asObject (valueLocator, "value locator").getProperty ("count") >= 1,
                     "value locator did not find slider value 25");

            auto hiddenLocator = readLocator ({ "--component-name", "editor.text", "--hidden" });
            require ((int) asObject (hiddenLocator, "hidden locator").getProperty ("count") == 1,
                     "hidden locator did not find editor.text before its tab was selected");

            auto hiddenWaitFailure = runCliExpectFailure ({ "-s", sessionName, "wait-for-locator", "--component-name", "editor.text", "--timeout-ms", "100" });
            require (hiddenWaitFailure.contains ("Timed out") || hiddenWaitFailure.contains ("Locator did not match"),
                     "wait-for-locator should default to visible components\n" + hiddenWaitFailure);
            runCli ({ "-s", sessionName, "wait-for-locator", "--component-name", "editor.text", "--hidden", "--timeout-ms", "500" });

            auto hiddenTextFailure = runCliExpectFailure ({ "-s", sessionName, "wait-for-text", "--exact", "Editor Page", "--timeout-ms", "100" });
            require (hiddenTextFailure.contains ("Timed out") || hiddenTextFailure.contains ("Text was not found"),
                     "wait-for-text should default to visible text\n" + hiddenTextFailure);
            runCli ({ "-s", sessionName, "wait-for-text", "--hidden", "--exact", "Editor Page", "--timeout-ms", "500" });

            auto disabledLocator = readLocator ({ "--component-name", "controls.disabled", "--disabled" });
            require ((int) asObject (disabledLocator, "disabled locator").getProperty ("count") == 1,
                     "disabled locator did not find controls.disabled");

            auto disabledClick = runCliExpectFailure ({ "-s", sessionName, "click", "--component-name", "controls.disabled", "--timeout-ms", "100" });
            require (disabledClick.contains ("target_disabled") || disabledClick.contains ("disabled"),
                     "disabled target should fail actionability\n" + disabledClick);

            auto trialClick = runCli ({ "-s", sessionName, "click", "--component-name", "nav.editor", "--trial" });
            require (trialClick.contains ("actionability"), "trial click should report actionability without executing\n" + trialClick);

            auto stillHiddenLocator = readLocator ({ "--component-name", "editor.text", "--hidden" });
            require ((int) asObject (stillHiddenLocator, "still hidden locator").getProperty ("count") == 1,
                     "trial click should not navigate to the editor page");

            auto strictFailure = runCliExpectFailure ({ "-s", sessionName, "click", "--role", "button", "--name", "Duplicate" });
            require (strictFailure.contains ("strict") || strictFailure.contains ("matched 2"),
                     "duplicate locator should fail strict mode\n" + strictFailure);

            auto deniedScreenshot = screenshotDirectory.getSiblingFile ("melatonin-automation-denied.png");
            auto deniedOutput = runCliExpectFailure ({ "-s", sessionName, "screenshot", "--target", "root", "--file", deniedScreenshot.getFullPathName() });
            require (deniedOutput.contains ("artifact root") || deniedOutput.contains ("artifact_path_denied"),
                     "screenshot outside artifact root should fail with artifact_path_denied\n" + deniedOutput);

            snapshot = readSnapshot();

            rootScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-root.png");
            runCli ({ "-s", sessionName, "screenshot", "--target", "root", "--file", rootScreenshot.getFullPathName() });
            assertPng (rootScreenshot, "root screenshot");

            auto nativeRootScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-root-native.png");
            runCli ({ "-s", sessionName, "screenshot", "--target", "root", "--source", "native", "--file", nativeRootScreenshot.getFullPathName(), "--no-base64" });
            assertPng (nativeRootScreenshot, "native root screenshot");

            auto invalidSourceOutput = runCliExpectFailure ({ "-s", sessionName, "screenshot", "--target", "root", "--source", "bogus", "--file", rootScreenshot.getFullPathName() });
            require (invalidSourceOutput.contains ("invalid_screenshot_source") || invalidSourceOutput.contains ("source must be"),
                     "invalid screenshot source should fail clearly\n" + invalidSourceOutput);

            auto buttonScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-button.png");
            auto editorButton = findByComponentName (snapshot, "nav.editor");
            auto editorButtonBounds = boundsOf (editorButton);
            runCli ({ "-s", sessionName, "screenshot", "--ref", asObject (editorButton, "nav.editor").getProperty ("ref").toString(), "--file", buttonScreenshot.getFullPathName() });
            assertPng (buttonScreenshot, "button screenshot");
            assertPngSize (buttonScreenshot, editorButtonBounds.getWidth(), editorButtonBounds.getHeight(), "button screenshot");

            auto locatorButtonScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-button-locator.png");
            runCli ({ "-s", sessionName, "screenshot", "--component-id", "nav.editor", "--file", locatorButtonScreenshot.getFullPathName(), "--no-base64" });
            assertPngSize (locatorButtonScreenshot, editorButtonBounds.getWidth(), editorButtonBounds.getHeight(), "locator button screenshot");

            auto clippedScreenshot = screenshotDirectory.getChildFile ("melatonin-automation-e2e-clip.png");
            runCli ({ "-s",
                      sessionName,
                      "screenshot",
                      "--target",
                      "root",
                      "--clip-x",
                      "0",
                      "--clip-y",
                      "0",
                      "--clip-w",
                      "50",
                      "--clip-h",
                      "40",
                      "--file",
                      clippedScreenshot.getFullPathName(),
                      "--no-base64" });
            assertPngSize (clippedScreenshot, 50, 40, "clipped screenshot");

            clickXYAtNode (findByComponentName (snapshot, "controls.power"));
            snapshot = readSnapshot();
            require (asObject (snapshot, "snapshot after click").getProperty ("stateHash").toString() != initialStateHash,
                     "clicking the power button should change the semantic stateHash");
            require ((bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("toggleState"),
                     "click-xy did not toggle the power button");
            assertStatus (snapshot, "Status: Power On");
            runCli ({ "-s", sessionName, "wait-for-ref", refByComponentName (snapshot, "controls.slider"), "--timeout-ms", "500" });
            runCli ({ "-s", sessionName, "wait-for-text", "Status: Power On", "--timeout-ms", "500" });
            runCli ({ "-s", sessionName, "wait-for-snapshot-change", "--state-hash", initialStateHash, "--timeout-ms", "500" });
            readLocator ({ "--component-id", "controls.slider" });
            runCli ({ "-s", sessionName, "wait-for-locator", "--component-id", "controls.slider", "--timeout-ms", "500" });

            runCli ({ "-s", sessionName, "uncheck", "--component-id", "controls.power" });
            snapshot = readSnapshot();
            require (! (bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("toggleState"),
                     "semantic uncheck did not clear the power button");

            runCli ({ "-s", sessionName, "check", "--component-id", "controls.power" });
            snapshot = readSnapshot();
            require ((bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("toggleState"),
                     "semantic check did not set the power button");
            assertStatus (snapshot, "Status: Power On");

            runCli ({ "-s", sessionName, "set-checked", "--component-id", "controls.power", "false" });
            snapshot = readSnapshot();
            require (! (bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("checked"),
                     "semantic set-checked false did not clear the power button");

            runCli ({ "-s", sessionName, "set-checked", "--component-id", "controls.power", "true" });
            snapshot = readSnapshot();
            require ((bool) asObject (findByComponentName (snapshot, "controls.power"), "controls.power").getProperty ("checked"),
                     "semantic set-checked true did not set the power button");
            assertStatus (snapshot, "Status: Power On");

            runCli ({ "-s", sessionName, "set-value", "--component-id", "controls.slider", "44" });
            snapshot = readSnapshot();
            require (juce::roundToInt (valueOf (findByComponentName (snapshot, "controls.slider"))) == 44,
                     "semantic set-value did not update the slider");
            assertStatus (snapshot, "Status: Slider 44");
            runCli ({ "-s", sessionName, "wait-for-value", "--component-id", "controls.slider", "--value", "44", "--timeout-ms", "500" });

            runCli ({ "-s", sessionName, "select-option", "--component-id", "controls.combo", "--text", "Beta" });
            snapshot = readSnapshot();
            require (asObject (findByComponentName (snapshot, "controls.combo"), "controls.combo").getProperty ("value").toString() == "Beta",
                     "semantic select-option did not select Beta");

            runCli ({ "-s", sessionName, "select-option", "--component-id", "controls.optionList", "--text", "Green" });
            snapshot = readSnapshot();
            require ((int) asObject (findByComponentName (snapshot, "controls.optionList"), "controls.optionList").getProperty ("selectedRow") == 1,
                     "ListBox metadata did not expose selected Green row");
            require (asObject (findByComponentName (snapshot, "controls.optionList"), "controls.optionList").getProperty ("selectedText").toString() == "Green",
                     "ListBox metadata did not expose selected Green text");
            assertStatus (snapshot, "Status: List Green");

            runCli ({ "-s", sessionName, "select-option", "--component-id", "controls.optionList", "--index", "2" });
            snapshot = readSnapshot();
            require ((int) asObject (findByComponentName (snapshot, "controls.optionList"), "controls.optionList").getProperty ("selectedRow") == 2,
                     "ListBox metadata did not expose selected Blue row");
            assertStatus (snapshot, "Status: List Blue");

            auto sliderBefore = valueOf (findByComponentName (snapshot, "controls.slider"));
            dragRef (refByComponentName (snapshot, "controls.slider"), 90, 0);
            snapshot = readSnapshot();
            auto sliderAfter = valueOf (findByComponentName (snapshot, "controls.slider"));
            require (sliderAfter > sliderBefore,
                     "slider drag did not increase value: " + juce::String (sliderBefore) + " -> " + juce::String (sliderAfter));
            assertStatus (snapshot, "Status: Slider");

            runCli ({ "-s", sessionName, "click", "--component-id", "nav.editor" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Editor");
            require (!findByComponentName (snapshot, "editor.text").isVoid(), "editor page did not expose its text editor");

            runCli ({ "-s", sessionName, "fill", "--component-id", "editor.text", "semantic fill" });
            snapshot = readSnapshot();
            clickRef (refByComponentName (snapshot, "editor.apply"));
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Applied semantic fill");

            auto editorRef = refByComponentName (snapshot, "editor.text");
            runCli ({ "-s", sessionName, "clear", editorRef });
            snapshot = readSnapshot();
            require (asObject (findByComponentName (snapshot, "editor.text"), "editor.text").getProperty ("value").toString().isEmpty(),
                     "semantic clear did not empty the editor");
            editorRef = refByComponentName (snapshot, "editor.text");
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

            runCli ({ "-s", sessionName, "select-tab", "--component-id", "advanced.tabs", "--name", "Actions" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Nested Actions");
            require (!findByComponentName (snapshot, "advanced.reset").isVoid(), "nested Actions tab did not expose Reset All");
            require (!findByComponentName (snapshot, "advanced.dragBox").isVoid(), "nested Actions tab did not expose Drag Box");

            auto dragBoxBefore = findByComponentName (snapshot, "advanced.dragBox");
            auto dragBoxBeforeBounds = boundsOf (dragBoxBefore);
            dragRef (asObject (dragBoxBefore, "advanced.dragBox").getProperty ("ref").toString(), 40, 15, 4);
            snapshot = readSnapshot();
            auto dragBoxAfterBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));
            require (dragBoxAfterBounds.getX() == dragBoxBeforeBounds.getX() + 40, "drag did not move Drag Box on the x axis");
            require (dragBoxAfterBounds.getY() == dragBoxBeforeBounds.getY() + 15, "drag did not move Drag Box on the y axis");
            assertStatus (snapshot, "steps=4");

            runCli ({ "-s", sessionName, "hover", juce::String (dragBoxAfterBounds.getCentreX()), juce::String (dragBoxAfterBounds.getCentreY()) });
            runCli ({ "-s", sessionName, "mouse-down", juce::String (dragBoxAfterBounds.getCentreX()), juce::String (dragBoxAfterBounds.getCentreY()) });
            runCli ({ "-s", sessionName, "mouse-up", juce::String (dragBoxAfterBounds.getCentreX()), juce::String (dragBoxAfterBounds.getCentreY()) });
            runCli ({ "-s", sessionName, "wheel", juce::String (dragBoxAfterBounds.getCentreX()), juce::String (dragBoxAfterBounds.getCentreY()), "--dy", "-1" });
            runCli ({ "-s",
                      sessionName,
                      "drag-xy",
                      juce::String (dragBoxAfterBounds.getCentreX()),
                      juce::String (dragBoxAfterBounds.getCentreY()),
                      juce::String (dragBoxAfterBounds.getCentreX() + 20),
                      juce::String (dragBoxAfterBounds.getCentreY() + 10),
                      "--steps",
                      "3" });
            snapshot = readSnapshot();
            auto dragBoxPointDragBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));
            require (dragBoxPointDragBounds.getX() == dragBoxAfterBounds.getX() + 20, "drag-xy did not move Drag Box on the x axis");
            require (dragBoxPointDragBounds.getY() == dragBoxAfterBounds.getY() + 10, "drag-xy did not move Drag Box on the y axis");
            assertStatus (snapshot, "steps=3");

            auto inputProbeBounds = boundsOf (findByComponentName (snapshot, "advanced.inputProbe"));
            auto expectedDragToBounds = dragBoxPointDragBounds.translated (inputProbeBounds.getCentreX() - dragBoxPointDragBounds.getCentreX(),
                                                                          inputProbeBounds.getCentreY() - dragBoxPointDragBounds.getCentreY());
            runCli ({ "-s",
                      sessionName,
                      "drag-to",
                      "--component-name",
                      "advanced.dragBox",
                      "--target-component-name",
                      "advanced.inputProbe",
                      "--steps",
                      "5" });
            snapshot = readSnapshot();
            auto dragBoxAfterDragToBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));
            require (dragBoxAfterDragToBounds.getX() == expectedDragToBounds.getX(), "drag-to did not move Drag Box to target x");
            require (dragBoxAfterDragToBounds.getY() == expectedDragToBounds.getY(), "drag-to did not move Drag Box to target y");
            assertStatus (snapshot, "steps=5");
            runCli ({ "-s", sessionName, "set-bounds", refByComponentName (snapshot, "advanced.dragBox"), "--x", "240", "--y", "100", "--w", "100", "--h", "42" });
            snapshot = readSnapshot();
            dragBoxAfterDragToBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));

            auto resetBoundsBeforeMcpDrag = boundsOf (findByComponentName (snapshot, "advanced.reset"));
            auto expectedMcpDragToBounds = dragBoxAfterDragToBounds.translated (resetBoundsBeforeMcpDrag.getCentreX() - dragBoxAfterDragToBounds.getCentreX(),
                                                                               resetBoundsBeforeMcpDrag.getCentreY() - dragBoxAfterDragToBounds.getCentreY());
            auto dragToOutput = runMcpBatch ({
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"juce_drag_to","arguments":{"session":"automation_fixture","locator":{"componentName":"advanced.dragBox"},"targetLocator":{"componentName":"advanced.reset"},"steps":2}}})"
            });
            auto dragToLines = juce::StringArray::fromLines (dragToOutput);
            require (dragToLines.size() >= 2, "MCP drag_to expected at least 2 response lines, got " + juce::String (dragToLines.size()) + "\n" + dragToOutput);
            assertMcpResult (parseMcpLine (dragToLines, 1), 2);
            snapshot = readSnapshot();
            auto dragBoxAfterMcpDragToBounds = boundsOf (findByComponentName (snapshot, "advanced.dragBox"));
            require (dragBoxAfterMcpDragToBounds.getX() == expectedMcpDragToBounds.getX(), "MCP drag_to did not move Drag Box to target x");
            require (dragBoxAfterMcpDragToBounds.getY() == expectedMcpDragToBounds.getY(), "MCP drag_to did not move Drag Box to target y");
            assertStatus (snapshot, "steps=2");
            runCli ({ "-s", sessionName, "set-bounds", refByComponentName (snapshot, "advanced.dragBox"), "--x", "240", "--y", "100", "--w", "100", "--h", "42" });
            snapshot = readSnapshot();

            runCli ({ "-s", sessionName, "dblclick", "--component-id", "advanced.inputProbe" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: DoubleClick");

            runCli ({ "-s", sessionName, "right-click", "--component-id", "advanced.inputProbe" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: RightClick");

            runCli ({ "-s", sessionName, "click", "--component-id", "advanced.inputProbe", "--click-count", "2" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: DoubleClick");

            runCli ({ "-s", sessionName, "click", "--component-id", "advanced.inputProbe", "--button", "right", "--position", "8,8" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: RightClick");

            auto invalidClickPosition = runCliExpectFailure ({ "-s", sessionName, "click", "--component-id", "advanced.inputProbe", "--position", "999,999" });
            require (invalidClickPosition.contains ("invalid_coordinate") || invalidClickPosition.contains ("outside target bounds"),
                     "click with an out-of-bounds target-local position should fail\n" + invalidClickPosition);

            runCli ({ "-s", sessionName, "press", "Control+K", "--component-id", "advanced.inputProbe" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Key Ctrl+K");

            runCli ({ "-s", sessionName, "key-down", "Shift+X", "--component-id", "advanced.inputProbe" });
            snapshot = readSnapshot();
            assertStatus (snapshot, "Status: Key Shift+X");
            runCli ({ "-s", sessionName, "key-up", "Shift+X", "--component-id", "advanced.inputProbe" });
            snapshot = readSnapshot();

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
                R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"juce_locator","arguments":{"session":"automation_fixture","locator":{"componentName":"controls.slider"}}}})",
                R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"juce_snapshot","arguments":{"session":"automation_fixture","format":"text","depth":12}}})"
            });

            auto lines = juce::StringArray::fromLines (output);
            require (lines.size() >= 5, "MCP smoke expected at least 5 response lines, got " + juce::String (lines.size()) + "\n" + output);

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
            bool foundLocatorTool = false;
            bool foundWaitForTextTool = false;
            bool foundDoubleClickTool = false;
            bool foundRightClickTool = false;
            bool foundKeyDownTool = false;
            bool foundKeyUpTool = false;
            bool foundClearTool = false;
            bool foundSetCheckedTool = false;
            bool foundDragToTool = false;

            for (const auto& toolInfo : *tools.getArray())
            {
                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_snapshot")
                    foundSnapshotTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_locator")
                    foundLocatorTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_wait_for_text")
                    foundWaitForTextTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_dblclick")
                    foundDoubleClickTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_right_click")
                    foundRightClickTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_key_down")
                    foundKeyDownTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_key_up")
                    foundKeyUpTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_clear")
                    foundClearTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_set_checked")
                    foundSetCheckedTool = true;

                if (asObject (toolInfo, "MCP tool").getProperty ("name").toString() == "juce_drag_to")
                    foundDragToTool = true;
            }

            require (foundSnapshotTool, "MCP tools/list did not expose juce_snapshot");
            require (foundLocatorTool, "MCP tools/list did not expose juce_locator");
            require (foundWaitForTextTool, "MCP tools/list did not expose juce_wait_for_text");
            require (foundDoubleClickTool, "MCP tools/list did not expose juce_dblclick");
            require (foundRightClickTool, "MCP tools/list did not expose juce_right_click");
            require (foundKeyDownTool, "MCP tools/list did not expose juce_key_down");
            require (foundKeyUpTool, "MCP tools/list did not expose juce_key_up");
            require (foundClearTool, "MCP tools/list did not expose juce_clear");
            require (foundSetCheckedTool, "MCP tools/list did not expose juce_set_checked");
            require (foundDragToTool, "MCP tools/list did not expose juce_drag_to");

            auto capabilitiesCallResult = assertMcpResult (parseMcpLine (lines, 2), 3);
            auto& capabilitiesCall = asObject (capabilitiesCallResult, "MCP capabilities result");
            auto capabilitiesContent = capabilitiesCall.getProperty ("content");
            require (capabilitiesContent.isArray() && !capabilitiesContent.getArray()->isEmpty(), "MCP capabilities did not return content");
            require (asObject (capabilitiesContent.getArray()->getReference (0), "MCP capabilities content").getProperty ("text").toString().contains ("protocolVersion"),
                     "MCP capabilities content did not include protocolVersion");

            auto locatorCallResult = assertMcpResult (parseMcpLine (lines, 3), 4);
            auto& locatorCall = asObject (locatorCallResult, "MCP locator result");
            auto locatorContent = locatorCall.getProperty ("content");
            require (locatorContent.isArray() && !locatorContent.getArray()->isEmpty(), "MCP locator did not return content");
            require (asObject (locatorContent.getArray()->getReference (0), "MCP locator content").getProperty ("text").toString().contains ("controls.slider"),
                     "MCP locator content did not include controls.slider");

            auto snapshotCallResult = assertMcpResult (parseMcpLine (lines, 4), 5);
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

        juce::var readLocator (std::initializer_list<juce::String> locatorArgs)
        {
            auto args = makeArgs ({ "-s", sessionName, "locator", "--format", "json" });
            args.addArray (makeArgs (locatorArgs));

            auto parsed = juce::JSON::parse (runCli (args));
            asObject (parsed, "locator");
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

        void dragRef (const juce::String& ref, int dx, int dy, int steps = 1)
        {
            runCli ({ "-s", sessionName, "drag", ref, "--dx", juce::String (dx), "--dy", juce::String (dy), "--steps", juce::String (steps) });
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
        class OptionsModel : public juce::ListBoxModel
        {
        public:
            int getNumRows() override { return options.size(); }

            juce::String getNameForRow (int rowNumber) override
            {
                return juce::isPositiveAndBelow (rowNumber, options.size()) ? options[rowNumber] : juce::String();
            }

            void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
            {
                g.fillAll (rowIsSelected ? juce::Colours::steelblue : juce::Colours::darkgrey);
                g.setColour (juce::Colours::white);
                g.drawText (getNameForRow (rowNumber), 8, 0, width - 16, height, juce::Justification::centredLeft);
            }

            void selectedRowsChanged (int lastRowSelected) override
            {
                if (onSelected && juce::isPositiveAndBelow (lastRowSelected, options.size()))
                    onSelected (options[lastRowSelected]);
            }

            juce::StringArray options { "Red", "Green", "Blue" };
            std::function<void (const juce::String&)> onSelected;
        };

        ControlsPage()
        {
            setName ("Controls Page");

            title.setText ("Controls Page", juce::dontSendNotification);
            title.setName ("controls.title");
            title.setComponentID ("controls.title");
            addAndMakeVisible (title);

            goEditor.setButtonText ("Go Editor");
            goEditor.setName ("nav.editor");
            goEditor.setComponentID ("nav.editor");
            addAndMakeVisible (goEditor);

            toggle.setButtonText ("Power Toggle");
            toggle.setName ("controls.power");
            toggle.setComponentID ("controls.power");
            addAndMakeVisible (toggle);

            slider.setName ("controls.slider");
            slider.setComponentID ("controls.slider");
            slider.setRange (0.0, 100.0, 1.0);
            slider.setValue (25.0);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 24);
            addAndMakeVisible (slider);

            combo.setName ("controls.combo");
            combo.setComponentID ("controls.combo");
            combo.addItem ("Alpha", 1);
            combo.addItem ("Beta", 2);
            combo.addItem ("Gamma", 3);
            combo.setSelectedId (1);
            addAndMakeVisible (combo);

            duplicateA.setButtonText ("Duplicate");
            duplicateA.setName ("controls.duplicateA");
            duplicateA.setComponentID ("controls.duplicateA");
            addAndMakeVisible (duplicateA);

            duplicateB.setButtonText ("Duplicate");
            duplicateB.setName ("controls.duplicateB");
            duplicateB.setComponentID ("controls.duplicateB");
            addAndMakeVisible (duplicateB);

            disabled.setButtonText ("Disabled Action");
            disabled.setName ("controls.disabled");
            disabled.setComponentID ("controls.disabled");
            disabled.setEnabled (false);
            addAndMakeVisible (disabled);

            optionList.setName ("controls.optionList");
            optionList.setComponentID ("controls.optionList");
            optionList.setModel (&optionsModel);
            optionList.setRowHeight (24);
            addAndMakeVisible (optionList);
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
            area.removeFromTop (10);
            duplicateA.setBounds (area.removeFromTop (30).removeFromLeft (140));
            duplicateB.setBounds (area.removeFromTop (30).removeFromLeft (140));
            area.removeFromTop (10);
            disabled.setBounds (area.removeFromTop (30).removeFromLeft (160));
            area.removeFromTop (10);
            optionList.setBounds (area.removeFromTop (82).removeFromLeft (180));
        }

        juce::TextButton goEditor;
        juce::ToggleButton toggle;
        juce::Slider slider;
        juce::ComboBox combo;
        juce::TextButton duplicateA;
        juce::TextButton duplicateB;
        juce::TextButton disabled;
        OptionsModel optionsModel;
        juce::ListBox optionList;

    private:
        juce::Label title;
    };

    class DragBox : public juce::Component
    {
    public:
        DragBox()
        {
            setName ("advanced.dragBox");
            setComponentID ("advanced.dragBox");
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
            dragEvents = 0;
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            ++dragEvents;
            setBounds (dragStartBounds.translated (event.getDistanceFromDragStartX(), event.getDistanceFromDragStartY()));

            if (onDragged)
                onDragged (getBounds(), dragEvents);
        }

        std::function<void (juce::Rectangle<int>, int)> onDragged;

    private:
        juce::Rectangle<int> dragStartBounds;
        int dragEvents = 0;
    };

    class InputProbe : public juce::Component
    {
    public:
        InputProbe()
        {
            setName ("advanced.inputProbe");
            setComponentID ("advanced.inputProbe");
            setWantsKeyboardFocus (true);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::darkblue);
            g.setColour (juce::Colours::white);
            g.drawFittedText ("Input Probe", getLocalBounds(), juce::Justification::centred, 1);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isRightButtonDown() && onRightClick)
                onRightClick();
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (onDoubleClick)
                onDoubleClick();
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (onKeyPressed)
                onKeyPressed (describeKey (key));

            return true;
        }

        std::function<void()> onDoubleClick;
        std::function<void()> onRightClick;
        std::function<void (const juce::String&)> onKeyPressed;

    private:
        static juce::String describeKey (const juce::KeyPress& key)
        {
            juce::String result;

            if (key.getModifiers().isCtrlDown())
                result << "Ctrl+";

            if (key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown())
                result << "Meta+";

            if (key.getModifiers().isAltDown())
                result << "Alt+";

            if (key.getModifiers().isShiftDown())
                result << "Shift+";

            auto code = key.getKeyCode();

            if (code > 0 && code < 128)
                result << juce::String::charToString ((juce::juce_wchar) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code));
            else
                result << juce::String (code);

            return result;
        }
    };

    class EditorPage : public juce::Component
    {
    public:
        EditorPage()
        {
            setName ("Editor Page");

            title.setText ("Editor Page", juce::dontSendNotification);
            title.setName ("editor.title");
            title.setComponentID ("editor.title");
            addAndMakeVisible (title);

            text.setName ("editor.text");
            text.setComponentID ("editor.text");
            text.setTextToShowWhenEmpty ("Type here", juce::Colours::grey);
            addAndMakeVisible (text);

            apply.setButtonText ("Apply Text");
            apply.setName ("editor.apply");
            apply.setComponentID ("editor.apply");
            addAndMakeVisible (apply);

            goAdvanced.setButtonText ("Go Advanced");
            goAdvanced.setName ("nav.advanced");
            goAdvanced.setComponentID ("nav.advanced");
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
            nestedTabs.setComponentID ("advanced.tabs");
            nestedTabs.addTab ("Metrics", juce::Colours::darkgrey, &metrics, false);
            nestedTabs.addTab ("Actions", juce::Colours::darkgrey, &actions, false);
            addAndMakeVisible (nestedTabs);

            metrics.setName ("Metrics Page");
            metricLabel.setText ("Metrics Ready", juce::dontSendNotification);
            metricLabel.setName ("advanced.metrics.label");
            metricLabel.setComponentID ("advanced.metrics.label");
            metrics.addAndMakeVisible (metricLabel);

            goActions.setButtonText ("Go Actions");
            goActions.setName ("advanced.goActions");
            goActions.setComponentID ("advanced.goActions");
            metrics.addAndMakeVisible (goActions);

            actions.setName ("Actions Page");
            reset.setButtonText ("Reset All");
            reset.setName ("advanced.reset");
            reset.setComponentID ("advanced.reset");
            actions.addAndMakeVisible (reset);

            actions.addAndMakeVisible (dragBox);
            actions.addAndMakeVisible (inputProbe);

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
            inputProbe.setBounds (360, 24, 120, 42);
        }

        CallbackTabbedComponent nestedTabs { juce::TabbedButtonBar::TabsAtTop };
        juce::TextButton goActions;
        juce::TextButton reset;
        DragBox dragBox;
        InputProbe inputProbe;
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
            status.setComponentID ("fixture.status");
            status.setText ("Status: Controls", juce::dontSendNotification);
            addAndMakeVisible (status);

            tabs.setName ("fixture.tabs");
            tabs.setComponentID ("fixture.tabs");
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

            controls.optionsModel.onSelected = [this] (const juce::String& option) {
                setStatus ("Status: List " + option);
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

            advanced.dragBox.onDragged = [this] (juce::Rectangle<int> bounds, int dragEvents) {
                setStatus ("Status: DragBox " + juce::String (bounds.getX()) + "," + juce::String (bounds.getY()) + " steps=" + juce::String (dragEvents));
            };

            advanced.inputProbe.onDoubleClick = [this] {
                setStatus ("Status: DoubleClick");
            };

            advanced.inputProbe.onRightClick = [this] {
                setStatus ("Status: RightClick");
            };

            advanced.inputProbe.onKeyPressed = [this] (const juce::String& key) {
                setStatus ("Status: Key " + key);
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
