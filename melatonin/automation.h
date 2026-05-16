#pragma once

#include "helpers/component_helpers.h"
#include <cstdlib>
#include <string>
#if JUCE_WINDOWS
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#ifndef MELATONIN_INSPECTOR_ENABLE_AUTOMATION
    #define MELATONIN_INSPECTOR_ENABLE_AUTOMATION 0
#endif

namespace melatonin
{
    struct AutomationOptions
    {
        juce::String sessionName;
        juce::String authToken;
        int port = 0;
        bool advertise = true;
        bool allowInput = true;
        bool allowMutation = true;
    };

#if MELATONIN_INSPECTOR_ENABLE_AUTOMATION
    class AutomationController : private juce::Thread
    {
    public:
        AutomationController (juce::Component& rootComponent, AutomationOptions automationOptions)
            : juce::Thread ("Melatonin Inspector Automation"),
              root (&rootComponent),
              options (std::move (automationOptions))
        {
            if (options.sessionName.isEmpty())
                options.sessionName = defaultSessionName();

            if (options.authToken.isEmpty())
                options.authToken = juce::Uuid().toString();

            listener = std::make_unique<juce::StreamingSocket>();

            if (!listener->createListener (options.port, "127.0.0.1"))
                return;

            boundPort = listener->getBoundPort();

            if (options.advertise)
                writeAdvertisement();

            startThread();
        }

        ~AutomationController() override
        {
            signalThreadShouldExit();

            if (listener)
                listener->close();

            waitForThreadToExit (2000);
            removeAdvertisement();
        }

        void updateRoot (juce::Component& newRoot)
        {
            root = &newRoot;

            if (options.advertise)
                writeAdvertisement();
        }

        void clearRoot()
        {
            root = nullptr;
        }

        [[nodiscard]] bool isRunning() const noexcept
        {
            return listener != nullptr && boundPort > 0;
        }

        [[nodiscard]] int getPort() const noexcept
        {
            return boundPort;
        }

        [[nodiscard]] juce::String getAuthToken() const
        {
            return options.authToken;
        }

    private:
        struct ComponentRef
        {
            juce::String ref;
            juce::Component::SafePointer<juce::Component> component;
        };

        juce::Component::SafePointer<juce::Component> root;
        AutomationOptions options;
        std::unique_ptr<juce::StreamingSocket> listener;
        int boundPort = -1;
        juce::File advertisementFile;
        juce::Array<ComponentRef> refs;
        int generation = 0;

        void run() override
        {
            while (!threadShouldExit() && listener != nullptr)
            {
                std::unique_ptr<juce::StreamingSocket> client (listener->waitForNextConnection());

                if (client == nullptr)
                    continue;

                handleClient (*client);
                client->close();
            }
        }

        void handleClient (juce::StreamingSocket& client)
        {
            auto requestLine = readLine (client);
            auto response = handleRequest (requestLine);
            writeLine (client, response);
        }

        juce::String readLine (juce::StreamingSocket& client)
        {
            std::string bytes;
            char buffer[1024] {};

            while (!threadShouldExit())
            {
                const auto ready = client.waitUntilReady (true, 5000);

                if (ready <= 0)
                    break;

                const auto bytesRead = client.read (buffer, (int) sizeof (buffer), false);

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

        static void writeLine (juce::StreamingSocket& client, const juce::String& line)
        {
            const auto payload = line + "\n";
            client.write (payload.toRawUTF8(), (int) payload.getNumBytesAsUTF8());
        }

        juce::String handleRequest (const juce::String& requestLine)
        {
            auto request = juce::JSON::parse (requestLine);
            auto* requestObject = request.getDynamicObject();

            if (requestObject == nullptr)
                return responseError ({}, "invalid_json", "Request must be a JSON object.");

            const auto id = getString (*requestObject, "id", {});

            if (getString (*requestObject, "token", {}) != options.authToken)
                return responseError (id, "unauthorized", "Invalid automation token.");

            const auto method = getString (*requestObject, "method", {});
            auto params = requestObject->getProperty ("params");
            auto* paramsObject = params.getDynamicObject();

            juce::DynamicObject emptyParams;

            if (paramsObject == nullptr)
                paramsObject = &emptyParams;

            if (method == "wait")
                juce::Thread::sleep (juce::jlimit (0, 30000, getInt (*paramsObject, "ms", 250)));

            auto result = callOnMessageThread ([this, method, paramsObject]() {
                return dispatch (method, *paramsObject);
            });

            if (auto* resultObject = result.getDynamicObject())
            {
                if (resultObject->getProperty ("__error").isString())
                    return responseError (id,
                                          resultObject->getProperty ("__error").toString(),
                                          resultObject->getProperty ("message").toString());
            }

            return responseOk (id, result);
        }

        juce::var dispatch (const juce::String& method, juce::DynamicObject& params)
        {
            if (method == "ping")
                return object ({ { "status", "ok" } });

            if (method == "snapshot")
                return snapshot (params);

            if (method == "screenshot")
                return screenshot (params);

            if (method == "click")
                return click (params);

            if (method == "click_xy")
                return clickXY (params);

            if (method == "type")
                return typeText (params);

            if (method == "press")
                return pressKey (params);

            if (method == "drag")
                return drag (params);

            if (method == "set_bounds")
                return setBounds (params);

            if (method == "set_property")
                return setProperty (params);

            if (method == "wait")
                return wait (params);

            return error ("unknown_method", "Unknown automation method: " + method);
        }

        juce::var snapshot (juce::DynamicObject& params)
        {
            if (root == nullptr)
                return error ("no_root", "No root component is attached.");

            refs.clear();
            ++generation;

            const auto maxDepth = getInt (params, "depth", 8);
            const auto format = getString (params, "format", "text");
            auto tree = serializeComponent (*root, 0, juce::jmax (0, maxDepth));

            juce::String text;
            appendTextSnapshot (text, tree, 0);

            if (format == "json")
                return object ({ { "generation", generation }, { "tree", tree }, { "text", text } });

            return object ({ { "generation", generation }, { "text", text } });
        }

        juce::var screenshot (juce::DynamicObject& params)
        {
            if (root == nullptr)
                return error ("no_root", "No root component is attached.");

            const auto ref = getString (params, "ref", {});
            auto* target = getTargetComponent (ref);
            auto targetName = getString (params, "target", {});

            if (ref.isNotEmpty() && target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            if (targetName == "root" || target == nullptr)
                target = root.getComponent();

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            auto image = target->createComponentSnapshot (target->getLocalBounds(), false, 1.0f);

            if (image.isNull())
                return error ("screenshot_failed", "Could not create component snapshot.");

            juce::MemoryBlock pngBytes;
            juce::MemoryOutputStream stream (pngBytes, false);
            juce::PNGImageFormat png;

            if (!png.writeImageToStream (image, stream))
                return error ("screenshot_failed", "Could not encode PNG.");

            const auto filePath = getString (params, "file", {});
            juce::String absolutePath;

            if (filePath.isNotEmpty())
            {
                auto file = juce::File (filePath).getFullPathName().isNotEmpty()
                                ? juce::File (filePath)
                                : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("melatonin-screenshot.png");

                file.getParentDirectory().createDirectory();

                if (!file.replaceWithData (pngBytes.getData(), pngBytes.getSize()))
                    return error ("screenshot_failed", "Could not write PNG file: " + file.getFullPathName());

                absolutePath = file.getFullPathName();
            }

            return object ({ { "mimeType", "image/png" },
                             { "width", image.getWidth() },
                             { "height", image.getHeight() },
                             { "file", absolutePath },
                             { "base64", juce::Base64::toBase64 (pngBytes.getData(), pngBytes.getSize()) } });
        }

        juce::var click (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto* target = requireTarget (params);

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            auto bounds = getRootBounds (*target);
            synthesizeClickAt (bounds.getCentre());
            return snapshotAfterAction();
        }

        juce::var clickXY (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            synthesizeClickAt ({ getInt (params, "x", 0), getInt (params, "y", 0) });
            return snapshotAfterAction();
        }

        juce::var typeText (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto* target = requireTarget (params);

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            target->grabKeyboardFocus();
            const auto text = getString (params, "text", {});

            if (auto* editor = dynamic_cast<juce::TextEditor*> (target))
            {
                editor->insertTextAtCaret (text);
            }
            else if (auto* label = dynamic_cast<juce::Label*> (target))
            {
                label->setText (label->getText() + text, juce::sendNotification);
            }
            else if (auto* peer = getRootPeer())
            {
                for (int i = 0; i < text.length(); ++i)
                    peer->handleKeyPress (0, text[i]);
            }

            return snapshotAfterAction();
        }

        juce::var pressKey (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto key = getString (params, "key", {});
            auto* target = getTargetComponent (getString (params, "ref", {}));

            if (target != nullptr)
                target->grabKeyboardFocus();

            if (auto* peer = getRootPeer())
            {
                const auto keyCode = keyCodeForName (key);
                const auto textCharacter = key.length() == 1 ? key[0] : juce::juce_wchar();
                peer->handleKeyPress (keyCode, textCharacter);
            }

            return snapshotAfterAction();
        }

        juce::var drag (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto* target = requireTarget (params);

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            auto start = getRootBounds (*target).getCentre();
            auto end = start.translated (getInt (params, "dx", 0), getInt (params, "dy", 0));

            if (auto* peer = getRootPeer())
            {
                auto peerStart = peer->getComponent().getLocalPoint (root, start).toFloat();
                auto peerEnd = peer->getComponent().getLocalPoint (root, end).toFloat();
                auto now = juce::Time::currentTimeMillis();
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerStart, juce::ModifierKeys(), 0.0f, 0.0f, now);
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerStart, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, now + 1);
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerEnd, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, now + 16);
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerEnd, juce::ModifierKeys(), 0.0f, 0.0f, now + 17);
            }

            return snapshotAfterAction();
        }

        juce::var setBounds (juce::DynamicObject& params)
        {
            if (!options.allowMutation)
                return error ("mutation_disabled", "Automation mutation is disabled for this session.");

            auto* target = requireTarget (params);

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            target->setBounds (getInt (params, "x", target->getX()),
                               getInt (params, "y", target->getY()),
                               getInt (params, "w", target->getWidth()),
                               getInt (params, "h", target->getHeight()));

            return snapshotAfterAction();
        }

        juce::var setProperty (juce::DynamicObject& params)
        {
            if (!options.allowMutation)
                return error ("mutation_disabled", "Automation mutation is disabled for this session.");

            auto* target = requireTarget (params);

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            const auto name = getString (params, "name", {});
            auto value = params.getProperty ("value");

            if (name == "visible")
                target->setVisible ((bool) value);
            else if (name == "enabled")
                target->setEnabled ((bool) value);
            else if (name == "opaque")
                target->setOpaque ((bool) value);
            else if (name == "alpha")
                target->setAlpha ((float) value);
            else if (name == "name")
                target->setName (value.toString());
            else if (name == "wantsFocus")
                target->setWantsKeyboardFocus ((bool) value);
            else
                target->getProperties().set (name, value);

            target->repaint();
            return snapshotAfterAction();
        }

        juce::var wait (juce::DynamicObject& params)
        {
            juce::ignoreUnused (params);
            return snapshotAfterAction();
        }

        juce::var snapshotAfterAction()
        {
            juce::DynamicObject params;
            params.setProperty ("format", "text");
            params.setProperty ("depth", 8);
            return snapshot (params);
        }

        juce::Component* requireTarget (juce::DynamicObject& params)
        {
            return getTargetComponent (getString (params, "ref", {}));
        }

        juce::Component* getTargetComponent (const juce::String& ref) const
        {
            if (ref.isEmpty())
                return nullptr;

            for (auto& entry : refs)
                if (entry.ref == ref)
                    return entry.component.getComponent();

            return nullptr;
        }

        juce::ComponentPeer* getRootPeer() const
        {
            return root != nullptr ? root->getPeer() : nullptr;
        }

        void synthesizeClickAt (juce::Point<int> rootPoint)
        {
            if (root == nullptr)
                return;

            if (auto* peer = getRootPeer())
            {
                auto peerPoint = peer->getComponent().getLocalPoint (root, rootPoint).toFloat();
                auto now = juce::Time::currentTimeMillis();
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerPoint, juce::ModifierKeys(), 0.0f, 0.0f, now);
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerPoint, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, now + 1);
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, peerPoint, juce::ModifierKeys(), 0.0f, 0.0f, now + 2);
            }
        }

        juce::Rectangle<int> getRootBounds (juce::Component& component) const
        {
            if (root == nullptr)
                return {};

            if (&component == root.getComponent())
                return root->getLocalBounds();

            if (auto* parent = component.getParentComponent())
                return root->getLocalArea (parent, component.getBounds());

            return {};
        }

        juce::var serializeComponent (juce::Component& component, int depth, int maxDepth)
        {
            auto* node = new juce::DynamicObject();
            auto ref = "m" + juce::String (refs.size() + 1);
            refs.add ({ ref, &component });

            node->setProperty ("ref", ref);
            node->setProperty ("name", componentString (&component));
            node->setProperty ("class", type (component));
            node->setProperty ("enabled", component.isEnabled());
            node->setProperty ("visible", component.isVisible());
            node->setProperty ("focused", component.hasKeyboardFocus (false));
            node->setProperty ("bounds", rectangleToVar (getRootBounds (component)));
            node->setProperty ("screenBounds", rectangleToVar (component.getScreenBounds()));

            if (component.isAccessible() && component.getAccessibilityHandler() != nullptr)
            {
                auto* handler = component.getAccessibilityHandler();
                node->setProperty ("role", accessibilityRoleName (handler->getRole()));
                node->setProperty ("title", handler->getTitle());

                if (handler->getValueInterface() != nullptr)
                    node->setProperty ("value", handler->getValueInterface()->getCurrentValueAsString());
            }

            if (auto* button = dynamic_cast<juce::Button*> (&component))
            {
                node->setProperty ("toggleable", button->isToggleable());
                node->setProperty ("toggleState", button->getToggleState());
            }

            juce::Array<juce::var> children;

            if (depth < maxDepth)
                addSerializedChildren (children, component, depth + 1, maxDepth);

            node->setProperty ("children", children);
            return node;
        }

        void addSerializedChildren (juce::Array<juce::var>& children, juce::Component& component, int depth, int maxDepth)
        {
            if (auto* multiPanel = dynamic_cast<juce::MultiDocumentPanel*> (&component))
            {
                if (auto* child = multiPanel->getCurrentTabbedComponent())
                    children.add (serializeComponent (*child, depth, maxDepth));

                return;
            }

            if (auto* tabs = dynamic_cast<juce::TabbedComponent*> (&component))
            {
                for (int i = 0; i < tabs->getNumTabs(); ++i)
                    if (auto* child = tabs->getTabContentComponent (i))
                        children.add (serializeComponent (*child, depth, maxDepth));

                return;
            }

            for (int i = 0; i < component.getNumChildComponents(); ++i)
            {
                auto* child = component.getChildComponent (i);

                if (child == nullptr || isInspectorInternalComponent (*child))
                    continue;

                children.add (serializeComponent (*child, depth, maxDepth));
            }
        }

        static bool isInspectorInternalComponent (juce::Component& component)
        {
            if (component.getName() == "Melatonin Overlay")
                return true;

            auto className = type (component);
            return className.contains ("melatonin::FPSMeter");
        }

        static void appendTextSnapshot (juce::String& out, const juce::var& node, int indent)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return;

            auto bounds = object->getProperty ("bounds").getDynamicObject();
            auto box = bounds != nullptr
                           ? bounds->getProperty ("x").toString() + "," + bounds->getProperty ("y").toString() + "," + bounds->getProperty ("w").toString() + "," + bounds->getProperty ("h").toString()
                           : "-";

            out << juce::String::repeatedString ("  ", indent)
                << "- " << object->getProperty ("class").toString()
                << " \"" << object->getProperty ("name").toString() << "\""
                << " [ref=" << object->getProperty ("ref").toString()
                << " box=" << box;

            auto role = object->getProperty ("role").toString();

            if (role.isNotEmpty())
                out << " role=" << role;

            auto value = object->getProperty ("value").toString();

            if (value.isNotEmpty())
                out << " value=\"" << value << "\"";

            if (!(bool) object->getProperty ("visible"))
                out << " hidden=true";

            if (!(bool) object->getProperty ("enabled"))
                out << " disabled=true";

            out << "]\n";

            auto children = object->getProperty ("children");

            if (children.isArray())
                for (auto& child : *children.getArray())
                    appendTextSnapshot (out, child, indent + 1);
        }

        static juce::var rectangleToVar (juce::Rectangle<int> rectangle)
        {
            return object ({ { "x", rectangle.getX() },
                             { "y", rectangle.getY() },
                             { "w", rectangle.getWidth() },
                             { "h", rectangle.getHeight() } });
        }

        static juce::String accessibilityRoleName (juce::AccessibilityRole role)
        {
            switch (role)
            {
                case juce::AccessibilityRole::button: return "button";
                case juce::AccessibilityRole::toggleButton: return "toggleButton";
                case juce::AccessibilityRole::radioButton: return "radioButton";
                case juce::AccessibilityRole::comboBox: return "comboBox";
                case juce::AccessibilityRole::image: return "image";
                case juce::AccessibilityRole::slider: return "slider";
                case juce::AccessibilityRole::label: return "label";
                case juce::AccessibilityRole::staticText: return "staticText";
                case juce::AccessibilityRole::editableText: return "editableText";
                case juce::AccessibilityRole::menuItem: return "menuItem";
                case juce::AccessibilityRole::menuBar: return "menuBar";
                case juce::AccessibilityRole::popupMenu: return "popupMenu";
                case juce::AccessibilityRole::table: return "table";
                case juce::AccessibilityRole::tableHeader: return "tableHeader";
                case juce::AccessibilityRole::column: return "column";
                case juce::AccessibilityRole::row: return "row";
                case juce::AccessibilityRole::cell: return "cell";
                case juce::AccessibilityRole::hyperlink: return "hyperlink";
                case juce::AccessibilityRole::list: return "list";
                case juce::AccessibilityRole::listItem: return "listItem";
                case juce::AccessibilityRole::tree: return "tree";
                case juce::AccessibilityRole::treeItem: return "treeItem";
                case juce::AccessibilityRole::progressBar: return "progressBar";
                case juce::AccessibilityRole::group: return "group";
                case juce::AccessibilityRole::dialogWindow: return "dialogWindow";
                case juce::AccessibilityRole::window: return "window";
                case juce::AccessibilityRole::scrollBar: return "scrollBar";
                case juce::AccessibilityRole::tooltip: return "tooltip";
                case juce::AccessibilityRole::splashScreen: return "splashScreen";
                case juce::AccessibilityRole::ignored: return "ignored";
                case juce::AccessibilityRole::unspecified: return "unspecified";
                default: break;
            }

            return "unknown";
        }

        static int keyCodeForName (juce::String key)
        {
            key = key.trim().toLowerCase();

            if (key == "tab") return juce::KeyPress::tabKey;
            if (key == "return" || key == "enter") return juce::KeyPress::returnKey;
            if (key == "escape" || key == "esc") return juce::KeyPress::escapeKey;
            if (key == "backspace") return juce::KeyPress::backspaceKey;
            if (key == "delete") return juce::KeyPress::deleteKey;
            if (key == "left") return juce::KeyPress::leftKey;
            if (key == "right") return juce::KeyPress::rightKey;
            if (key == "up") return juce::KeyPress::upKey;
            if (key == "down") return juce::KeyPress::downKey;
            if (key.length() == 1) return key[0];

            return 0;
        }

        juce::var callOnMessageThread (std::function<juce::var()> function)
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                return function();

            struct Call
            {
                std::function<juce::var()> function;
                juce::var result;
            } call { std::move (function), {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* data) -> void* {
                auto* c = static_cast<Call*> (data);
                c->result = c->function();
                return nullptr;
            }, &call);

            return call.result;
        }

        static juce::var object (std::initializer_list<std::pair<juce::String, juce::var>> properties)
        {
            auto* result = new juce::DynamicObject();

            for (const auto& property : properties)
                result->setProperty (property.first, property.second);

            return result;
        }

        static juce::var error (const juce::String& code, const juce::String& message)
        {
            return object ({ { "__error", code }, { "message", message } });
        }

        static juce::String responseOk (const juce::String& id, const juce::var& result)
        {
            return juce::JSON::toString (object ({ { "id", id }, { "ok", true }, { "result", result } }), true);
        }

        static juce::String responseError (const juce::String& id, const juce::String& code, const juce::String& message)
        {
            return juce::JSON::toString (object ({ { "id", id },
                                                   { "ok", false },
                                                   { "error", object ({ { "code", code }, { "message", message } }) } }),
                                         true);
        }

        static juce::String getString (juce::DynamicObject& object, const juce::Identifier& name, const juce::String& fallback)
        {
            auto value = object.getProperty (name);
            return value.isVoid() ? fallback : value.toString();
        }

        static int getInt (juce::DynamicObject& object, const juce::Identifier& name, int fallback)
        {
            auto value = object.getProperty (name);
            return value.isVoid() ? fallback : (int) value;
        }

        static juce::String defaultSessionName()
        {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                return app->getApplicationName();

            return "melatonin";
        }

        static juce::File sessionsDirectory()
        {
            const char* temp = std::getenv (
#if JUCE_WINDOWS
                "TEMP"
#else
                "TMPDIR"
#endif
            );

            auto directory = juce::File (temp != nullptr ? juce::String::fromUTF8 (temp) : juce::String ("/tmp"))
                                 .getChildFile ("melatonin_inspector")
                                 .getChildFile ("sessions");
            directory.createDirectory();
            return directory;
        }

        void writeAdvertisement()
        {
            auto sessionFileName = options.sessionName.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");

            if (sessionFileName.isEmpty())
                sessionFileName = "session";

            advertisementFile = sessionsDirectory().getChildFile (juce::String (currentProcessId()) + "-" + sessionFileName + ".json");

            auto* data = new juce::DynamicObject();
            data->setProperty ("pid", currentProcessId());
            data->setProperty ("session", options.sessionName);
            data->setProperty ("root", root != nullptr ? componentString (root.getComponent()) : juce::String());
            data->setProperty ("host", "127.0.0.1");
            data->setProperty ("port", boundPort);
            data->setProperty ("token", options.authToken);
            data->setProperty ("createdAt", juce::Time::getCurrentTime().toISO8601 (true));

            advertisementFile.replaceWithText (juce::JSON::toString (juce::var (data), true));
        }

        static int currentProcessId()
        {
#if JUCE_WINDOWS
            return (int) ::GetCurrentProcessId();
#else
            return (int) ::getpid();
#endif
        }

        void removeAdvertisement()
        {
            if (advertisementFile.existsAsFile())
                advertisementFile.deleteFile();
        }
    };
#endif
}
