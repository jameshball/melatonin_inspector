#include <JuceHeader.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <set>
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

            exerciseAdditionalDemoCoverage();

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
            require ((int) traceStopObject.getProperty ("events") >= 100, "DemoRunner trace did not record enough events");
            copyEvidenceFile (traceStopObject.getProperty ("trace").toString(), "demorunner-trace.json");
        }

    private:
        juce::File demoRunnerPath;
        juce::File cliPath;
        juce::File evidenceDirectory;
        juce::ChildProcess demoRunner;
        juce::StringArray capturedCategories;

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

        bool tryRunCli (std::initializer_list<juce::String> args, int timeoutMs = 3000)
        {
            auto argArray = makeArgs (args);
            juce::StringArray command;
            command.add (cliPath.getFullPathName());
            command.addArray (argArray);

            return runProcess (command, "melatonin-ui " + argArray.joinIntoString (" "), false, timeoutMs).exitCode == 0;
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

        juce::var readWindows()
        {
            auto parsed = juce::JSON::parse (runCli ({ "-s", sessionName, "windows" }));
            asObject (parsed, "windows");
            return parsed;
        }

        int windowCount()
        {
            auto windows = asObject (readWindows(), "windows").getProperty ("windows");
            return windows.isArray() ? windows.getArray()->size() : 0;
        }

        juce::String secondaryWindowIdContaining (const juce::String& titleText)
        {
            auto windows = asObject (readWindows(), "windows").getProperty ("windows");
            require (windows.isArray(), "windows did not return an array");

            for (const auto& window : *windows.getArray())
            {
                auto* object = window.getDynamicObject();

                if (object == nullptr)
                    continue;

                const auto id = object->getProperty ("id").toString();
                const auto title = object->getProperty ("title").toString();

                if (id != "root" && title.contains (titleText))
                    return id;
            }

            fail ("Could not find secondary window containing title: " + titleText);
            return {};
        }

        juce::var readLocator (std::initializer_list<juce::String> locatorArgs)
        {
            auto args = makeArgs ({ "-s", sessionName, "locator", "--format", "json" });
            args.addArray (makeArgs (locatorArgs));

            auto parsed = juce::JSON::parse (runCli (args));
            asObject (parsed, "locator");
            return parsed;
        }

        juce::var firstLocatorMatch (std::initializer_list<juce::String> locatorArgs, const juce::String& label)
        {
            auto locator = readLocator (locatorArgs);
            auto matches = asObject (locator, label + " locator").getProperty ("matches");
            require (matches.isArray() && !matches.getArray()->isEmpty(), label + " locator returned no matches");
            return matches.getArray()->getReference (0);
        }

        int locatorCount (std::initializer_list<juce::String> locatorArgs, const juce::String& label)
        {
            return (int) asObject (readLocator (locatorArgs), label + " locator").getProperty ("count");
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

        static juce::Rectangle<int> boundsOf (const juce::var& node)
        {
            auto bounds = asObject (node, "node").getProperty ("bounds");
            auto& boundsObject = asObject (bounds, "bounds");

            return { (int) boundsObject.getProperty ("x"),
                     (int) boundsObject.getProperty ("y"),
                     (int) boundsObject.getProperty ("w"),
                     (int) boundsObject.getProperty ("h") };
        }

        static int nodeInt (const juce::var& node, const juce::Identifier& property)
        {
            return (int) asObject (node, "node").getProperty (property);
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

        juce::var visibleNodeByClass (const juce::var& snapshot, const juce::String& className)
        {
            return findSnapshotNode (snapshot, className, [&] (juce::DynamicObject& node) {
                return isVisible (node) && hasClass (node, className);
            });
        }

        juce::var visibleNodeContainingText (const juce::var& snapshot, const juce::String& text)
        {
            return findSnapshotNode (snapshot, text, [&] (juce::DynamicObject& node) {
                const auto haystack = node.getProperty ("name").toString()
                                      + " " + node.getProperty ("title").toString()
                                      + " " + node.getProperty ("value").toString();

                return isVisible (node) && haystack.contains (text);
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
            scrollListItemIntoView (name);
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--role", "listItem", "--name", name, "--exact", "--force", "--timeout-ms", "3000" });
            juce::Thread::sleep (500);
        }

        void scrollListItemIntoView (const juce::String& name)
        {
            tryRunCli ({ "-s", sessionName, "select-option", "--class", "juce::ListBox", "--exact", "--text", name, "--timeout-ms", "1000" }, 3000);

            for (int listIndex = 0; listIndex < 4; ++listIndex)
            {
                tryRunCli ({ "-s", sessionName, "select-option", "--class", "juce::ListBox", "--exact", "--nth", juce::String (listIndex), "--text", name, "--timeout-ms", "1000" }, 3000);

                if (tryRunCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "300" }, 1000))
                    return;
            }

            if (tryRunCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "300" }, 1000))
                return;

            for (int attempt = 0; attempt < 80; ++attempt)
            {
                if (tryRunCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", name, "--exact", "--timeout-ms", "300" }, 1000))
                    return;

                runCli ({ "-s", sessionName, "wheel", "231", "300", "--dy", attempt < 40 ? "-12" : "12" });
                runCli ({ "-s", sessionName, "wait", "--ms", "100" });
            }

            require (false, "Could not scroll list item into view: " + name);
        }

        static juce::String categoryScreenshotName (const juce::String& category)
        {
            return category.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789") + "-category.png";
        }

        void openCategory (const juce::String& category)
        {
            openDemosPanel();

            if (!tryRunCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", category, "--exact", "--timeout-ms", "500" }, 1500))
                tryRunCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Previous", "--exact", "--timeout-ms", "1000" }, 3000);

            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "listItem", "--name", category, "--exact", "--timeout-ms", "3000" });
            clickVisibleListItem (category);

            if (!capturedCategories.contains (category))
            {
                captureScreenshot (categoryScreenshotName (category));
                capturedCategories.add (category);
            }
        }

        void selectDemoFromCategory (const juce::String& category, const juce::String& demoFile)
        {
            openCategory (category);
            clickVisibleListItem (demoFile);
            runCli ({ "-s", sessionName, "wait", "--ms", "750" });
        }

        void assertCodeTabLoads()
        {
            selectTopLevelTab ("Code");
            runCli ({ "-s", sessionName, "wait-for-text", "CodeContent", "--timeout-ms", "3000" });
            selectTopLevelTab ("Demo");
        }

        void selectDemoLocalTab (const juce::String& tabName)
        {
            auto tabs = visibleNodeByClass (readSnapshot (8), "DemoTabbedComponent");
            runCli ({ "-s", sessionName, "select-tab", nodeRef (tabs), "--name", tabName });
            runCli ({ "-s", sessionName, "wait", "--ms", "250" });
        }

        void clickWindowPoint (juce::Rectangle<int> bounds, int relativeX, int relativeY, const juce::String& target = "root")
        {
            runCli ({ "-s", sessionName, "click-xy",
                      juce::String (bounds.getX() + relativeX),
                      juce::String (bounds.getY() + relativeY),
                      "--target", target });
        }

        void clickVisibleText (const juce::String& text)
        {
            auto node = visibleNodeContainingText (readSnapshot (10), text);
            auto bounds = boundsOf (node);
            clickWindowPoint (bounds, bounds.getWidth() / 2, bounds.getHeight() / 2);
            runCli ({ "-s", sessionName, "wait", "--ms", "250" });
        }

        struct DemoCase
        {
            enum class Exercise
            {
                codeEditor,
                componentGrid,
                componentTransforms,
                dialogs,
                grid,
                images,
                fonts,
                audioSettings,
                gain,
                valueTrees,
                xmlAndJson,
                openGL,
                widgets,
                menus,
                windows,
                mdi,
                properties,
                keyMappings,
                openGL2D
            };

            const char* category = nullptr;
            const char* file = nullptr;
            const char* screenshot = nullptr;
            Exercise exercise = Exercise::codeEditor;
        };

        void exerciseAdditionalDemoCoverage()
        {
            const DemoCase demos[] {
                { "GUI", "CodeEditorDemo.h",          "code-editor-demo.png", DemoCase::Exercise::codeEditor },
                { "GUI", "ComponentDemo.h",           "component-demo.png", DemoCase::Exercise::componentGrid },
                { "GUI", "ComponentTransformsDemo.h", "component-transforms-demo.png", DemoCase::Exercise::componentTransforms },
                { "GUI", "DialogsDemo.h",             "dialogs-demo.png", DemoCase::Exercise::dialogs },
                { "GUI", "GridDemo.h",                "grid-demo.png", DemoCase::Exercise::grid },
                { "GUI", "ImagesDemo.h",              "images-demo.png", DemoCase::Exercise::images },
                { "GUI", "FontsDemo.h",               "fonts-demo.png", DemoCase::Exercise::fonts },
                { "GUI", "WidgetsDemo.h",             "widgets-demo.png", DemoCase::Exercise::widgets },
                { "GUI", "MenusDemo.h",               "menus-demo.png", DemoCase::Exercise::menus },
                { "GUI", "WindowsDemo.h",             "windows-demo.png", DemoCase::Exercise::windows },
                { "GUI", "MDIDemo.h",                 "mdi-demo.png", DemoCase::Exercise::mdi },
                { "GUI", "PropertiesDemo.h",          "properties-demo.png", DemoCase::Exercise::properties },
                { "GUI", "KeyMappingsDemo.h",         "key-mappings-demo.png", DemoCase::Exercise::keyMappings },
                { "Audio", "AudioSettingsDemo.h",     "audio-settings-demo.png", DemoCase::Exercise::audioSettings },
                { "DSP", "GainDemo.h",                "gain-demo.png", DemoCase::Exercise::gain },
                { "Utilities", "ValueTreesDemo.h",    "value-trees-demo.png", DemoCase::Exercise::valueTrees },
                { "Utilities", "XMLandJSONDemo.h",    "xml-and-json-demo.png", DemoCase::Exercise::xmlAndJson },
                { "GUI", "OpenGLDemo.h",              "opengl-demo.png", DemoCase::Exercise::openGL },
                { "GUI", "OpenGLDemo2D.h",            "opengl-2d-demo.png", DemoCase::Exercise::openGL2D }
            };

            for (const auto& demo : demos)
            {
                selectDemoFromCategory (demo.category, demo.file);
                assertCodeTabLoads();
                captureScreenshot (demo.screenshot);
                exerciseDemo (demo);
            }
        }

        void exerciseDemo (const DemoCase& demo)
        {
            switch (demo.exercise)
            {
                case DemoCase::Exercise::codeEditor:           exerciseCodeEditorDemo(); break;
                case DemoCase::Exercise::componentGrid:        exerciseComponentDemo(); break;
                case DemoCase::Exercise::componentTransforms:  exerciseComponentTransformsDemo(); break;
                case DemoCase::Exercise::dialogs:              exerciseDialogsDemo(); break;
                case DemoCase::Exercise::grid:                 exerciseGridDemo(); break;
                case DemoCase::Exercise::images:               exerciseImagesDemo(); break;
                case DemoCase::Exercise::fonts:                exerciseFontsDemo(); break;
                case DemoCase::Exercise::audioSettings:        exerciseAudioSettingsDemo(); break;
                case DemoCase::Exercise::gain:                 exerciseGainDemo(); break;
                case DemoCase::Exercise::valueTrees:           exerciseValueTreesDemo(); break;
                case DemoCase::Exercise::xmlAndJson:           exerciseXmlAndJsonDemo(); break;
                case DemoCase::Exercise::openGL:               exerciseOpenGLDemo(); break;
                case DemoCase::Exercise::widgets:              exerciseWidgetsDemo(); break;
                case DemoCase::Exercise::menus:                exerciseMenusDemo(); break;
                case DemoCase::Exercise::windows:              exerciseWindowsDemo(); break;
                case DemoCase::Exercise::mdi:                  exerciseMdiDemo(); break;
                case DemoCase::Exercise::properties:           exercisePropertiesDemo(); break;
                case DemoCase::Exercise::keyMappings:          exerciseKeyMappingsDemo(); break;
                case DemoCase::Exercise::openGL2D:             exerciseOpenGL2DDemo(); break;
            }
        }

        void exerciseCodeEditorDemo()
        {
            auto before = captureScreenshot ("code-editor-before-type.png");
            runCli ({ "-s", sessionName, "click", "--role", "editableText", "--nth", "0", "--force", "--timeout-ms", "3000" });

            for (int i = 0; i < 8; ++i)
                runCli ({ "-s", sessionName, "press", "return", "--role", "editableText", "--nth", "0", "--force" });

            auto after = captureScreenshot ("code-editor-after-type.png");
            assertScreenshotsDiffer (before, after, "CodeEditor keyboard editing", 4);
        }

        void exerciseComponentDemo()
        {
            captureScreenshot ("component-before-grid-resize.png");
            auto lightBounds = boundsOf (firstLocatorMatch ({ "--class", "ToggleLightComponent", "--nth", "0", "--visible" },
                                                            "ComponentDemo light"));
            runCli ({ "-s", sessionName, "mouse-move", "830", "580" });
            runCli ({ "-s", sessionName, "mouse-move", juce::String (lightBounds.getCentreX()), juce::String (lightBounds.getCentreY()) });
            runCli ({ "-s", sessionName, "wait", "--ms", "150" });

            auto grid = firstLocatorMatch ({ "--class", "ToggleLightGridComponent", "--nth", "0", "--visible" },
                                           "ComponentDemo light grid");
            auto bounds = boundsOf (grid);
            runCli ({ "-s", sessionName, "set-bounds", nodeRef (grid),
                      "--x", juce::String (bounds.getX()),
                      "--y", juce::String (bounds.getY()),
                      "--w", juce::String (bounds.getWidth() - 120),
                      "--h", juce::String (bounds.getHeight() - 80) });
            runCli ({ "-s", sessionName, "wait", "--ms", "150" });
            auto afterBounds = boundsOf (firstLocatorMatch ({ "--class", "ToggleLightGridComponent", "--nth", "0", "--visible" },
                                                            "ComponentDemo resized light grid"));
            require (afterBounds.getWidth() == bounds.getWidth() - 120 && afterBounds.getHeight() == bounds.getHeight() - 80,
                     "ComponentDemo light-grid bounds did not update");
            captureScreenshot ("component-after-grid-resize.png");
        }

        void exerciseComponentTransformsDemo()
        {
            auto before = captureScreenshot ("component-transforms-before-drag.png");
            auto dragger = firstLocatorMatch ({ "--class", "CornerDragger", "--nth", "0", "--visible" },
                                              "ComponentTransforms dragger");
            auto beforeBounds = boundsOf (dragger);
            runCli ({ "-s", sessionName, "drag", nodeRef (dragger), "--dx", "70", "--dy", "45", "--steps", "5" });
            auto afterBounds = boundsOf (firstLocatorMatch ({ "--class", "CornerDragger", "--nth", "0", "--visible" },
                                                           "ComponentTransforms moved dragger"));
            require (afterBounds.getCentre().getDistanceFrom (beforeBounds.getCentre()) > 20.0f,
                     "ComponentTransforms dragger did not move");
            auto after = captureScreenshot ("component-transforms-after-drag.png");
            assertScreenshotsDiffer (before, after, "ComponentTransforms drag");
        }

        void exerciseDialogsDemo()
        {
            runCli ({ "-s", sessionName, "uncheck", "--role", "toggleButton", "--name", "Use Native Windows", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "toggleButton", "--name", "Use Native Windows", "--exact", "--value", "false", "--timeout-ms", "3000" });
            captureScreenshot ("dialogs-before-alert-window.png");

            const auto initialWindowCount = windowCount();
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Alert Window With Extra Components", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-text", "AlertWindow demo..", "--timeout-ms", "3000" });
            require (windowCount() > initialWindowCount, "Opening a non-native AlertWindow did not add an automation window");

            auto dialogId = secondaryWindowIdContaining ("AlertWindow demo");
            captureScreenshot ("dialogs-extra-components-window.png", { "--target", dialogId, "--source", "component" });

            runCli ({ "-s", sessionName, "fill", "--class", "juce::TextEditor", "--value", "enter some text here", "--exact", "--visible", "Automation dialog input" });
            runCli ({ "-s", sessionName, "wait-for-value", "--class", "juce::TextEditor", "--visible", "--value", "Automation dialog input", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "select-option", "--class", "juce::ComboBox", "--value", "option 1", "--exact", "--visible", "--text", "option 3" });
            runCli ({ "-s", sessionName, "wait-for-value", "--class", "juce::ComboBox", "--visible", "--value", "option 3", "--exact", "--timeout-ms", "3000" });
            captureScreenshot ("dialogs-extra-components-filled.png", { "--target", dialogId, "--source", "component" });

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "OK", "--exact", "--visible", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-text", "Automation dialog input", "--timeout-ms", "3000" });

            dialogId = secondaryWindowIdContaining ("Alert Box");
            captureScreenshot ("dialogs-result-window.png", { "--target", dialogId, "--source", "component" });
            auto resultOk = firstLocatorMatch ({ "--role", "button", "--name", "OK", "--exact", "--visible" },
                                               "Dialogs result OK button");
            auto okBounds = boundsOf (resultOk);
            runCli ({ "-s", sessionName, "click-xy", juce::String (okBounds.getCentreX()), juce::String (okBounds.getCentreY()), "--target", dialogId });
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "button", "--name", "Alert Window With Extra Components", "--exact", "--visible", "--timeout-ms", "3000" });
            require (windowCount() == initialWindowCount, "Dismissing non-native AlertWindows did not restore the original window count");
        }

        void exerciseGridDemo()
        {
            auto before = captureScreenshot ("grid-before-resize.png");
            auto grid = firstLocatorMatch ({ "--class", "GridDemo", "--nth", "0", "--visible" },
                                           "GridDemo root");
            auto bounds = boundsOf (grid);
            runCli ({ "-s", sessionName, "set-bounds", nodeRef (grid),
                      "--x", juce::String (bounds.getX()),
                      "--y", juce::String (bounds.getY()),
                      "--w", juce::String (bounds.getWidth() - 180),
                      "--h", juce::String (bounds.getHeight() - 140) });
            runCli ({ "-s", sessionName, "wait", "--ms", "250" });
            auto after = captureScreenshot ("grid-after-resize.png");
            assertScreenshotsDiffer (before, after, "GridDemo responsive resize");
        }

        void exerciseImagesDemo()
        {
            auto before = captureScreenshot ("images-before-resize-bar-drag.png");
            auto resizer = firstLocatorMatch ({ "--class", "StretchableLayoutResizerBar", "--nth", "0", "--visible" },
                                              "ImagesDemo resizer");
            runCli ({ "-s", sessionName, "drag", nodeRef (resizer), "--dx", "0", "--dy", "120", "--steps", "5" });
            auto after = captureScreenshot ("images-after-resize-bar-drag.png");
            assertScreenshotsDiffer (before, after, "ImagesDemo resizer drag");
        }

        void exerciseFontsDemo()
        {
            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--nth", "0", "Automation fonts demo" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "editableText", "--nth", "0", "--value", "Automation fonts demo", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "Bold", "--exact" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "toggleButton", "--name", "Bold", "--exact", "--value", "true", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "Italic", "--exact" });
            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "32" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "32", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--nth", "1", "--text", "Right" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "1", "--value", "Right", "--timeout-ms", "3000" });
            captureScreenshot ("fonts-after-controls.png");
        }

        void exerciseWidgetsDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-locator", "--class", "DemoTabbedComponent", "--visible", "--timeout-ms", "3000" });

            runCli ({ "-s", sessionName, "check", "--role", "radioButton", "--name", "Radio Button #3", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "radioButton", "--name", "Radio Button #3", "--exact", "--value", "true", "--timeout-ms", "3000" });
            captureScreenshot ("widgets-buttons-after-radio.png");

            selectDemoLocalTab ("Sliders");
            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "25" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "25", "--timeout-ms", "3000" });
            captureScreenshot ("widgets-sliders-before-drag.png");
            auto slider = firstLocatorMatch ({ "--role", "slider", "--nth", "2", "--visible" },
                                             "Widgets horizontal slider");
            const auto beforeSliderValue = asObject (slider, "Widgets horizontal slider").getProperty ("value").toString().getDoubleValue();
            runCli ({ "-s", sessionName, "drag", nodeRef (slider), "--dx", "90", "--dy", "0", "--steps", "5" });
            auto draggedSlider = firstLocatorMatch ({ "--role", "slider", "--nth", "2", "--visible" },
                                                    "Widgets dragged horizontal slider");
            const auto afterSliderValue = asObject (draggedSlider, "Widgets dragged horizontal slider").getProperty ("value").toString().getDoubleValue();
            require (std::abs (afterSliderValue - beforeSliderValue) > 0.001, "Widgets horizontal slider drag did not change its semantic value");
            captureScreenshot ("widgets-sliders-after-drag.png");

            selectDemoLocalTab ("Toolbars");
            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "90" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "90", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Vertical/Horizontal", "--exact", "--timeout-ms", "3000" });
            captureScreenshot ("widgets-toolbar-after-orientation.png");

            selectDemoLocalTab ("Misc");
            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--value", "Single-line text box", "--exact", "Automation widgets misc" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "editableText", "--nth", "0", "--value", "Automation widgets misc", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--value", "combo box item 1", "--exact", "--text", "combo box item 4" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--value", "combo box item 4", "--exact", "--timeout-ms", "3000" });
            captureScreenshot ("widgets-misc-after-edit.png");

            selectDemoLocalTab ("Menus");
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Short", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "menuItem", "--name", "Single Item", "--exact", "--visible", "--timeout-ms", "3000" });
            captureScreenshot ("widgets-popup-menu.png");
            runCli ({ "-s", sessionName, "press", "escape", "--timeout-ms", "3000" });

            selectDemoLocalTab ("Tables");
            runCli ({ "-s", sessionName, "select-option", "--class", "TableListBox", "--nth", "0", "--index", "2", "--timeout-ms", "3000" });
            auto table = findSnapshotNode (readSnapshot (8), "Widgets table", [] (juce::DynamicObject& node) {
                return isVisible (node) && hasClass (node, "TableListBox");
            });
            require (nodeInt (table, "selectedRow") == 2, "Widgets table did not select row 2");
            captureScreenshot ("widgets-table-after-select.png");

            selectDemoLocalTab ("Drag & Drop");
            auto dragBefore = captureScreenshot ("widgets-dragdrop-before.png");
            auto sourceList = firstLocatorMatch ({ "--class", "ListBox", "--nth", "0", "--visible" },
                                                 "Widgets drag source list");
            auto target = firstLocatorMatch ({ "--class", "DragAndDropDemoTarget", "--nth", "0", "--visible" },
                                             "Widgets drag target");
            auto sourceBounds = boundsOf (sourceList);
            auto targetBounds = boundsOf (target);
            runCli ({ "-s", sessionName, "drag-xy",
                      juce::String (sourceBounds.getX() + 35),
                      juce::String (sourceBounds.getY() + 18),
                      juce::String (targetBounds.getCentreX()),
                      juce::String (targetBounds.getCentreY()),
                      "--steps", "8" });
            auto dragAfter = captureScreenshot ("widgets-dragdrop-after.png");
            assertScreenshotsDiffer (dragBefore, dragAfter, "Widgets drag-and-drop", 4);
        }

        void exerciseMenusDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-text", "Menu Position", "--timeout-ms", "3000" });

            auto before = captureScreenshot ("menus-before-popup.png");
            runCli ({ "-s", sessionName, "press", "command+g", "--class", "MenusDemo", "--exact", "--force", "--timeout-ms", "3000" });
            auto afterOuter = captureScreenshot ("menus-after-outer-green.png");
            assertScreenshotsDiffer (before, afterOuter, "Menus outer colour command");

            runCli ({ "-s", sessionName, "press", "shift+command+r", "--class", "MenusDemo", "--exact", "--force", "--timeout-ms", "3000" });
            captureScreenshot ("menus-after-inner-red.png");
        }

        void exerciseWindowsDemo()
        {
            const auto initialWindowCount = windowCount();
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Show Windows", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-text", "Dialog Windows can be used", "--timeout-ms", "3000" });
            require (windowCount() >= initialWindowCount + 4, "WindowsDemo did not open the expected secondary windows");
            captureScreenshot ("windows-after-show-all.png");

            auto alertId = secondaryWindowIdContaining ("Alert Window");
            captureScreenshot ("windows-alert-window.png", { "--target", alertId, "--source", "component" });
            runCli ({ "-s", sessionName, "fill", "--class", "juce::TextEditor", "--value", "Text editor", "--exact", "--visible", "Automation window text" });
            runCli ({ "-s", sessionName, "wait-for-value", "--class", "juce::TextEditor", "--nth", "0", "--value", "Automation window text", "--exact", "--visible", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "select-option", "--class", "juce::ComboBox", "--value", "Combo box", "--exact", "--visible", "--text", "Item 3" });
            runCli ({ "-s", sessionName, "wait-for-value", "--class", "juce::ComboBox", "--value", "Item 3", "--exact", "--visible", "--timeout-ms", "3000" });
            captureScreenshot ("windows-alert-filled.png", { "--target", alertId, "--source", "component" });
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Button 2", "--exact", "--visible", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-text", "Dismissed the Alert Window using Button 2", "--timeout-ms", "3000" });

            auto dialogId = secondaryWindowIdContaining ("Dialog Window");
            captureScreenshot ("windows-dialog-window.png", { "--target", dialogId, "--source", "component" });
            runCli ({ "-s", sessionName, "press", "escape", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "500" });

            if (windowCount() > initialWindowCount)
            {
                runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Close Windows", "--exact", "--force", "--timeout-ms", "3000" });
                runCli ({ "-s", sessionName, "wait", "--ms", "750" });
            }

            require (windowCount() == initialWindowCount, "WindowsDemo Close Windows did not restore the original window count");
            captureScreenshot ("windows-after-close-all.png");
        }

        void exerciseMdiDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-text", "Show with tabs", "--timeout-ms", "3000" });
            auto panel = visibleNodeByClass (readSnapshot (8), "DemoMultiDocumentPanel");
            require (nodeInt (panel, "documentCount") >= 1, "MDIDemo did not expose initial document count");
            require (nodeString (panel, "layoutMode") == "floating", "MDIDemo did not start in floating window mode");

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Create a new note", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Create a new note", "--exact", "--timeout-ms", "3000" });
            panel = visibleNodeByClass (readSnapshot (8), "DemoMultiDocumentPanel");
            require (nodeInt (panel, "documentCount") >= 3, "MDIDemo did not create additional notes");
            captureScreenshot ("mdi-after-create-notes.png");

            runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "Show with tabs", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "toggleButton", "--name", "Show with tabs", "--exact", "--value", "true", "--timeout-ms", "3000" });
            panel = visibleNodeByClass (readSnapshot (8), "DemoMultiDocumentPanel");
            require (nodeString (panel, "layoutMode") == "tabs", "MDIDemo did not switch to tabbed layout");
            captureScreenshot ("mdi-tabbed-layout.png");

            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--nth", "0", "Automation MDI note" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "editableText", "--nth", "0", "--value", "Automation MDI note", "--timeout-ms", "3000" });
            captureScreenshot ("mdi-after-edit-note.png");

            const auto countBeforeClose = nodeInt (panel, "documentCount");
            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Close active document", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "500" });
            panel = visibleNodeByClass (readSnapshot (8), "DemoMultiDocumentPanel");
            require (nodeInt (panel, "documentCount") == countBeforeClose - 1, "MDIDemo did not close the active document");
            captureScreenshot ("mdi-after-close-active.png");
        }

        void exercisePropertiesDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-text", "Text Editors", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "500" });
            runCli ({ "-s", sessionName, "fill", "--role", "editableText", "--value", "This is a single-line Text Property", "--exact", "Automation property value" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "editableText", "--nth", "0", "--value", "Automation property value", "--exact", "--timeout-ms", "3000" });
            captureScreenshot ("properties-text-after-fill.png");

            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--nth", "0", "--text", "Item 5" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "0", "--value", "Item 5", "--timeout-ms", "3000" });
            captureScreenshot ("properties-choice-after-select.png");

            runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--nth", "0", "--force", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "toggleButton", "--nth", "0", "--value", "true", "--timeout-ms", "3000" });
            captureScreenshot ("properties-toggle-after-check.png");

            runCli ({ "-s", sessionName, "click-xy", "35", "235" });
            runCli ({ "-s", sessionName, "wait-for-locator", "--role", "slider", "--visible", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "--force", "64" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "64", "--timeout-ms", "3000" });
            captureScreenshot ("properties-slider-after-set.png");
        }

        void exerciseKeyMappingsDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-locator", "--class", "KeyPressTarget", "--visible", "--timeout-ms", "3000" });
            auto target = visibleNodeByClass (readSnapshot (8), "KeyPressTarget");
            auto button = findNode (target, [] (juce::DynamicObject& node) {
                return isVisible (node) && hasClass (node, "TextButton");
            });
            require (! button.isVoid(), "Could not find KeyMappings target button");
            auto beforeBounds = boundsOf (button);
            auto before = captureScreenshot ("key-mappings-before-keys.png");

            runCli ({ "-s", sessionName, "press", "shift+g", "--class", "KeyPressTarget", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "press", "shift+b", "--class", "KeyPressTarget", "--timeout-ms", "3000" });
            target = visibleNodeByClass (readSnapshot (8), "KeyPressTarget");
            button = findNode (target, [] (juce::DynamicObject& node) {
                return isVisible (node) && hasClass (node, "TextButton");
            });
            require (! button.isVoid(), "Could not find moved KeyMappings target button");
            auto afterBounds = boundsOf (button);
            require (afterBounds.getX() > beforeBounds.getX() && afterBounds.getY() > beforeBounds.getY(),
                     "KeyMappings arrow key commands did not move the target button");

            runCli ({ "-s", sessionName, "press", "command+g", "--class", "KeyPressTarget", "--timeout-ms", "3000" });
            auto after = captureScreenshot ("key-mappings-after-keys.png");
            assertScreenshotsDiffer (before, after, "KeyMappings key commands", 4);
        }

        void exerciseAudioSettingsDemo()
        {
            auto before = captureScreenshot ("audio-settings-before-toggle.png");

            if (locatorCount ({ "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible" }, "AudioSettings MIDI toggle buttons") > 0)
            {
                auto toggle = firstLocatorMatch ({ "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible" },
                                                 "AudioSettings MIDI toggle");
                const auto wasChecked = (bool) asObject (toggle, "AudioSettings MIDI toggle").getProperty ("checked");

                if (wasChecked)
                    runCli ({ "-s", sessionName, "uncheck", "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible", "--force", "--timeout-ms", "3000" });
                else
                    runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible", "--force", "--timeout-ms", "3000" });

                auto updated = firstLocatorMatch ({ "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible" },
                                                  "AudioSettings MIDI toggle after click");
                const auto isChecked = (bool) asObject (updated, "AudioSettings MIDI toggle after click").getProperty ("checked");
                require (isChecked != wasChecked, "AudioSettings MIDI toggle did not change state");

                auto after = captureScreenshot ("audio-settings-after-toggle.png");
                assertScreenshotsDiffer (before, after, "AudioSettings MIDI toggle", 3);

                if (wasChecked)
                    runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible", "--force", "--timeout-ms", "3000" });
                else
                    runCli ({ "-s", sessionName, "uncheck", "--role", "toggleButton", "--name", "IAC Driver Bus 1", "--visible", "--force", "--timeout-ms", "3000" });
            }
            else
            {
                auto diagnostics = firstLocatorMatch ({ "--class", "juce::TextEditor", "--nth", "0", "--visible" },
                                                      "AudioSettings diagnostics editor");
                auto bounds = boundsOf (diagnostics);
                runCli ({ "-s", sessionName, "click-xy", juce::String (bounds.getCentreX()), juce::String (bounds.getCentreY()) });
                captureScreenshot ("audio-settings-after-diagnostics-click.png");
            }
        }

        void exerciseGainDemo()
        {
            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "6" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "6", "--timeout-ms", "3000" });
            auto before = captureScreenshot ("gain-before-slider-drag.png");
            auto slider = firstLocatorMatch ({ "--role", "slider", "--nth", "0", "--visible" },
                                             "GainDemo slider");
            runCli ({ "-s", sessionName, "drag", nodeRef (slider), "--dx", "-120", "--dy", "0", "--steps", "5" });
            auto after = captureScreenshot ("gain-after-slider-drag.png");
            assertScreenshotsDiffer (before, after, "GainDemo slider drag");
        }

        void exerciseValueTreesDemo()
        {
            auto before = captureScreenshot ("value-trees-before-delete.png");
            auto beforeCount = locatorCount ({ "--role", "treeItem", "--visible" }, "ValueTrees tree items");
            require (beforeCount > 4, "ValueTrees demo did not expose enough tree items");

            runCli ({ "-s", sessionName, "click", "--role", "treeItem", "--nth", "4", "--visible", "--force", "--timeout-ms", "3000" });
            require (locatorCount ({ "--role", "treeItem", "--selected", "--visible" }, "ValueTrees selected tree item") == 1,
                     "ValueTrees tree item click did not select exactly one visible item");

            runCli ({ "-s", sessionName, "press", "backspace", "--role", "treeItem", "--selected", "--visible", "--force" });
            auto afterDeleteCount = locatorCount ({ "--role", "treeItem", "--visible" }, "ValueTrees tree items after delete");
            require (afterDeleteCount < beforeCount, "ValueTrees delete did not reduce the visible tree item count");
            auto afterDelete = captureScreenshot ("value-trees-after-delete.png");
            assertScreenshotsDiffer (before, afterDelete, "ValueTrees delete", 4);

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Undo", "--exact", "--timeout-ms", "3000" });
            auto afterUndoCount = locatorCount ({ "--role", "treeItem", "--visible" }, "ValueTrees tree items after undo");
            require (afterUndoCount >= beforeCount, "ValueTrees undo did not restore the deleted tree item");
            captureScreenshot ("value-trees-after-undo.png");
        }

        void exerciseXmlAndJsonDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "0", "--value", "XML", "--timeout-ms", "3000" });
            auto before = captureScreenshot ("xml-json-before-type-select.png");
            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--nth", "0", "--text", "JSON" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "0", "--value", "JSON", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--role", "editableText", "--nth", "0", "--force", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "press", "backspace", "--role", "editableText", "--nth", "0", "--force" });
            auto after = captureScreenshot ("xml-json-after-json-edit.png");
            assertScreenshotsDiffer (before, after, "XML/JSON select and edit");
        }

        void exerciseOpenGLDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-text", "Shader Preset:", "--timeout-ms", "7000" });
            runCli ({ "-s", sessionName, "check", "--role", "toggleButton", "--name", "Draw 2D graphics in background", "--exact", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "toggleButton", "--name", "Draw 2D graphics in background", "--exact", "--value", "true", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "1000" });

            captureScreenshot ("opengl-demo-component.png", { "--source", "component" });
            auto nativeRoot = captureScreenshot ("opengl-demo-native.png", { "--source", "native" });
            assertScreenshotHasVariation (nativeRoot, "OpenGL native root screenshot", 18, 15);

            auto nativeScene = captureScreenshot ("opengl-demo-native-scene.png",
                                                  { "--source", "native",
                                                    "--clip-x", "170",
                                                    "--clip-y", "145",
                                                    "--clip-w", "500",
                                                    "--clip-h", "270" });
            assertScreenshotHasVariation (nativeScene, "OpenGL native clipped scene screenshot", 18, 15);
        }

        void exerciseOpenGL2DDemo()
        {
            runCli ({ "-s", sessionName, "wait-for-text", "Shader Preset:", "--timeout-ms", "7000" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "0", "--value", "Simple Gradient", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "1000" });

            auto before = captureScreenshot ("opengl-2d-native-before.png", { "--source", "native" });
            assertScreenshotHasVariation (before, "OpenGL2D native screenshot before", 18, 15);

            runCli ({ "-s", sessionName, "select-option", "--role", "comboBox", "--nth", "0", "--text", "Solid Colour" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "comboBox", "--nth", "0", "--value", "Solid Colour", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "click", "--class", "CodeEditorComponent", "--nth", "0", "--force", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "press", "backspace", "--class", "CodeEditorComponent", "--nth", "0", "--force", "--timeout-ms", "3000" });
            runCli ({ "-s", sessionName, "wait", "--ms", "1000" });

            auto after = captureScreenshot ("opengl-2d-native-after.png", { "--source", "native" });
            assertScreenshotHasVariation (after, "OpenGL2D native screenshot after", 18, 15);
            assertScreenshotsDiffer (before, after, "OpenGL2D shader selection/edit", 8);
        }

        void exerciseAccessibilityDemo()
        {
            auto snapshot = readSnapshot (8);
            auto demoTabs = visibleNodeByClassAndName (snapshot, "juce::TabbedComponent", "Demo tabs");
            auto tabNames = asObject (demoTabs, "Demo tabs").getProperty ("tabNames");
            require (tabNames.isArray() && tabNames.getArray()->size() >= 2, "Accessibility demo tabs did not expose tab names");

            runCli ({ "-s", sessionName, "click", "--role", "button", "--name", "Press me!", "--exact" });

            runCli ({ "-s", sessionName, "check", "--role", "radioButton", "--name", "Button 2", "--exact" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "radioButton", "--name", "Button 2", "--exact", "--value", "true", "--timeout-ms", "3000" });

            runCli ({ "-s", sessionName, "set-value", "--role", "slider", "--nth", "0", "42" });
            runCli ({ "-s", sessionName, "wait-for-value", "--role", "slider", "--nth", "0", "--value", "42", "--timeout-ms", "3000" });

            demoTabs = visibleNodeByClassAndName (readSnapshot (8), "juce::TabbedComponent", "Demo tabs");
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

        juce::File captureScreenshot (const juce::String& name, std::initializer_list<juce::String> screenshotArgs = {})
        {
            runCli ({ "-s", sessionName, "wait", "--ms", "250" });
            auto args = makeArgs ({ "-s", sessionName, "screenshot", "--file", name, "--no-base64" });
            args.addArray (makeArgs (screenshotArgs));

            auto outputPath = runCli (args);
            auto screenshot = juce::File (outputPath);
            require (screenshot.existsAsFile() && screenshot.getSize() > 1000,
                     "Screenshot was not written or is too small: " + outputPath);
            copyEvidenceFile (outputPath, name);
            return evidenceDirectory.getChildFile (name);
        }

        void assertScreenshotHasVariation (const juce::File& file,
                                           const juce::String& label,
                                           int minimumUniqueColours,
                                           int minimumLuminanceRange)
        {
            auto image = juce::ImageFileFormat::loadFrom (file);
            require (!image.isNull(), label + " could not be decoded: " + file.getFullPathName());

            std::set<int> colours;
            auto minLuminance = 255;
            auto maxLuminance = 0;
            auto samples = 0;
            const auto stepX = juce::jmax (1, image.getWidth() / 32);
            const auto stepY = juce::jmax (1, image.getHeight() / 32);

            for (int y = 0; y < image.getHeight(); y += stepY)
            {
                for (int x = 0; x < image.getWidth(); x += stepX)
                {
                    const auto colour = image.getPixelAt (x, y);

                    if (colour.getAlpha() == 0)
                        continue;

                    const auto red = (int) colour.getRed();
                    const auto green = (int) colour.getGreen();
                    const auto blue = (int) colour.getBlue();
                    const auto luminance = (red * 2126 + green * 7152 + blue * 722) / 10000;

                    colours.insert ((red << 16) | (green << 8) | blue);
                    minLuminance = juce::jmin (minLuminance, luminance);
                    maxLuminance = juce::jmax (maxLuminance, luminance);
                    ++samples;
                }
            }

            require (samples > 20, label + " did not contain enough opaque pixels");

            const auto luminanceRange = maxLuminance - minLuminance;
            require ((int) colours.size() >= minimumUniqueColours && luminanceRange >= minimumLuminanceRange,
                     label + " looks blank or too flat: uniqueColours=" + juce::String ((int) colours.size())
                         + " luminanceRange=" + juce::String (luminanceRange));
        }

        void assertScreenshotsDiffer (const juce::File& before,
                                      const juce::File& after,
                                      const juce::String& label,
                                      int minimumDifferentSamples = 12)
        {
            auto beforeImage = juce::ImageFileFormat::loadFrom (before);
            auto afterImage = juce::ImageFileFormat::loadFrom (after);

            require (!beforeImage.isNull(), label + " before image could not be decoded: " + before.getFullPathName());
            require (!afterImage.isNull(), label + " after image could not be decoded: " + after.getFullPathName());

            if (beforeImage.getBounds() != afterImage.getBounds())
                return;

            auto differentSamples = 0;
            const auto stepX = juce::jmax (1, beforeImage.getWidth() / 160);
            const auto stepY = juce::jmax (1, beforeImage.getHeight() / 120);

            for (int y = 0; y < beforeImage.getHeight(); y += stepY)
            {
                for (int x = 0; x < beforeImage.getWidth(); x += stepX)
                {
                    if (beforeImage.getPixelAt (x, y) != afterImage.getPixelAt (x, y))
                    {
                        ++differentSamples;

                        if (differentSamples >= minimumDifferentSamples)
                            return;
                    }
                }
            }

            require (false, label + " did not visibly change enough: differentSamples=" + juce::String (differentSamples));
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
