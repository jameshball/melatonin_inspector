#include <melatonin_inspector/melatonin_inspector.h>

namespace
{
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
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        inspector = std::make_unique<melatonin::Inspector> (mainWindow->content);

#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
        melatonin::AutomationOptions options;
        options.sessionName = "automation_fixture";
        inspector->enableAutomation (options);
#endif

        inspector->setVisible (true);
        juce::Process::makeForegroundProcess();
        mainWindow->toFront (true);
    }

    void shutdown() override
    {
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
};

START_JUCE_APPLICATION (AutomationFixtureApp)
