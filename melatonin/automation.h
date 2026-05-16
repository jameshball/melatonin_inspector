#pragma once

#include "helpers/component_helpers.h"
#include <atomic>
#include <cstdlib>
#include <string>
#if JUCE_WINDOWS
    #include <windows.h>
#else
    #include <sys/stat.h>
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
        bool allowFileWrite = true;
        juce::File artifactRoot;
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

            {
                const juce::ScopedLock lock (activeClientLock);

                if (activeClient != nullptr)
                    activeClient->close();
            }

            waitForThreadToStop();
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

        struct TargetResolution
        {
            juce::Component* component = nullptr;
            juce::var error;
        };

        juce::Component::SafePointer<juce::Component> root;
        AutomationOptions options;
        std::unique_ptr<juce::StreamingSocket> listener;
        juce::CriticalSection activeClientLock;
        juce::StreamingSocket* activeClient = nullptr;
        int boundPort = -1;
        juce::File advertisementFile;
        juce::File traceFile;
        juce::Array<juce::var> traceEvents;
        juce::Array<ComponentRef> refs;
        int generation = 0;
        bool traceEnabled = false;
        static constexpr int protocolVersion = 1;

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
            {
                const juce::ScopedLock lock (activeClientLock);
                activeClient = &client;
            }

            auto requestLine = readLine (client);
            auto response = handleRequest (requestLine);
            writeLine (client, response);

            {
                const juce::ScopedLock lock (activeClientLock);

                if (activeClient == &client)
                    activeClient = nullptr;
            }
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
            auto* data = payload.toRawUTF8();
            auto bytesRemaining = (int) payload.getNumBytesAsUTF8();

            while (bytesRemaining > 0)
            {
                const auto bytesWritten = client.write (data, bytesRemaining);

                if (bytesWritten <= 0)
                    break;

                data += bytesWritten;
                bytesRemaining -= bytesWritten;
            }
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

            auto result = dispatchRequest (method, *paramsObject);

            recordTraceEvent (method, result);

            if (auto* resultObject = result.getDynamicObject())
            {
                if (resultObject->getProperty ("__error").isString())
                    return responseError (id,
                                          resultObject->getProperty ("__error").toString(),
                                          resultObject->getProperty ("message").toString());
            }

            return responseOk (id, result);
        }

        juce::var dispatchRequest (const juce::String& method, juce::DynamicObject& params)
        {
            if (method == "wait")
                sleepUntilReadyOrStopped (juce::jlimit (0, 30000, getInt (params, "ms", 250)));

            if (isWaitForMethod (method))
                return waitForCondition (method, params);

            if (!isAutoWaitMethod (method))
                return callOnMessageThread ([this, method, &params]() {
                    return dispatch (method, params);
                });

            const auto deadline = juce::Time::currentTimeMillis() + juce::jlimit (0, 30000, getInt (params, "timeoutMs", 0));
            juce::var lastResult;

            do
            {
                lastResult = callOnMessageThread ([this, method, &params]() {
                    return dispatch (method, params);
                });

                if (!isRetryableActionabilityError (lastResult))
                    return lastResult;

                sleepUntilReadyOrStopped (50);
            } while (juce::Time::currentTimeMillis() < deadline && !threadShouldExit());

            if (auto* errorObject = lastResult.getDynamicObject())
                return error ("operation_timeout", "Timed out waiting for actionability: " + errorObject->getProperty ("message").toString());

            return error ("operation_timeout", "Timed out waiting for actionability.");
        }

        static bool isWaitForMethod (const juce::String& method)
        {
            return method == "wait_for_ref"
                   || method == "wait_for_locator"
                   || method == "wait_for_text"
                   || method == "wait_for_value"
                   || method == "wait_for_snapshot_change";
        }

        juce::var waitForCondition (const juce::String& method, juce::DynamicObject& params)
        {
            const auto deadline = juce::Time::currentTimeMillis() + juce::jlimit (0, 30000, getInt (params, "timeoutMs", 5000));
            juce::var lastResult;

            do
            {
                lastResult = callOnMessageThread ([this, method, &params]() {
                    return evaluateWaitCondition (method, params);
                });

                if (!isError (lastResult))
                    return lastResult;

                sleepUntilReadyOrStopped (50);
            } while (juce::Time::currentTimeMillis() < deadline && !threadShouldExit());

            if (auto* errorObject = lastResult.getDynamicObject())
                return error ("operation_timeout", "Timed out waiting: " + errorObject->getProperty ("message").toString());

            return error ("operation_timeout", "Timed out waiting.");
        }

        juce::var evaluateWaitCondition (const juce::String& method, juce::DynamicObject& params)
        {
            if (method == "wait_for_ref")
            {
                auto* target = getTargetComponent (getString (params, "ref", {}));
                return target != nullptr ? snapshotAfterAction()
                                         : error ("wait_not_ready", "Ref is not available.");
            }

            if (method == "wait_for_locator")
            {
                auto result = resolveLocatorQuery (params, true, false);

                if (isError (result))
                    return result;

                auto* object = result.getDynamicObject();
                return object != nullptr && (int) object->getProperty ("count") > 0
                           ? result
                           : error ("wait_not_ready", "Locator has no matches.");
            }

            if (method == "wait_for_text")
            {
                const auto expected = getString (params, "text", {});

                if (expected.isEmpty())
                    return error ("invalid_text", "wait_for_text requires non-empty text.");

                juce::DynamicObject snapshotParams;
                snapshotParams.setProperty ("format", "json");
                snapshotParams.setProperty ("depth", getInt (params, "depth", 12));
                auto result = snapshot (snapshotParams);
                auto* snapshotObject = result.getDynamicObject();
                auto tree = snapshotObject != nullptr ? snapshotObject->getProperty ("tree") : juce::var();

                return treeContainsText (tree, expected, (bool) params.getProperty ("exact"), params.getProperty ("visible"))
                           ? AutomationController::object ({ { "text", expected } })
                           : error ("wait_not_ready", "Text was not found.");
            }

            if (method == "wait_for_value")
            {
                auto resolution = resolveTarget (params, true, true);

                if (!resolution.error.isVoid())
                    return resolution.error;

                auto expected = getString (params, "value", {});
                auto current = semanticValueFor (*resolution.component);

                return matchesString (current, expected, (bool) params.getProperty ("exact"))
                           ? object ({ { "value", current } })
                           : error ("wait_not_ready", "Value did not match. Current value: " + current);
            }

            if (method == "wait_for_snapshot_change")
            {
                juce::DynamicObject snapshotParams;
                snapshotParams.setProperty ("format", "text");
                snapshotParams.setProperty ("depth", getInt (params, "depth", 12));
                auto result = snapshot (snapshotParams);
                auto* object = result.getDynamicObject();

                return object != nullptr && object->getProperty ("stateHash").toString() != getString (params, "stateHash", {})
                           ? result
                           : error ("wait_not_ready", "Snapshot state hash has not changed.");
            }

            return error ("unknown_method", "Unknown wait method: " + method);
        }

        static bool isAutoWaitMethod (const juce::String& method)
        {
            return method == "click"
                   || method == "dblclick"
                   || method == "right_click"
                   || method == "hover"
                   || method == "mouse_move"
                   || method == "mouse_down"
                   || method == "mouse_up"
                   || method == "wheel"
                   || method == "drag_xy"
                   || method == "type"
                   || method == "fill"
                   || method == "clear"
                   || method == "press"
                   || method == "key_down"
                   || method == "key_up"
                   || method == "check"
                   || method == "uncheck"
                   || method == "set_checked"
                   || method == "set_value"
                   || method == "select_option"
                   || method == "select_tab"
                   || method == "drag"
                   || method == "screenshot"
                   || method == "set_bounds"
                   || method == "set_property";
        }

        static bool isRetryableActionabilityError (const juce::var& result)
        {
            auto* resultObject = result.getDynamicObject();

            if (resultObject == nullptr || !resultObject->getProperty ("__error").isString())
                return false;

            auto code = resultObject->getProperty ("__error").toString();
            return code == "locator_not_found"
                   || code == "target_not_showing"
                   || code == "target_disabled"
                   || code == "target_empty_bounds"
                   || code == "target_not_receiving_events";
        }

        juce::var dispatch (const juce::String& method, juce::DynamicObject& params)
        {
            if (method == "ping")
                return object ({ { "status", "ok" } });

            if (method == "capabilities")
                return capabilities();

            if (method == "snapshot")
                return snapshot (params);

            if (method == "locator")
                return locator (params);

            if (method == "count")
                return count (params);

            if (method == "describe")
                return describe (params);

            if (method == "windows")
                return windows();

            if (method == "trace_start")
                return traceStart (params);

            if (method == "trace_stop")
                return traceStop();

            if (method == "screenshot")
                return screenshot (params);

            if (method == "click")
                return click (params);

            if (method == "dblclick")
                return doubleClick (params);

            if (method == "right_click")
                return rightClick (params);

            if (method == "click_xy")
                return clickXY (params);

            if (method == "hover" || method == "mouse_move")
                return mouseMove (params);

            if (method == "mouse_down")
                return mouseButton (params, true);

            if (method == "mouse_up")
                return mouseButton (params, false);

            if (method == "wheel")
                return wheel (params);

            if (method == "drag_xy")
                return dragXY (params);

            if (method == "type")
                return typeText (params);

            if (method == "fill")
                return fill (params);

            if (method == "clear")
                return clear (params);

            if (method == "press")
                return pressKey (params);

            if (method == "key_down")
                return keyDown (params);

            if (method == "key_up")
                return keyUp (params);

            if (method == "check")
                return check (params, true);

            if (method == "uncheck")
                return check (params, false);

            if (method == "set_checked")
                return setChecked (params);

            if (method == "set_value")
                return setValue (params);

            if (method == "select_option")
                return selectOption (params);

            if (method == "select_tab")
                return selectTab (params);

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

        juce::var capabilities() const
        {
            return object ({ { "protocolVersion", protocolVersion },
                             { "session", options.sessionName },
                             { "features", object ({ { "locators", true },
                                                     { "actionability", true },
                                                     { "semanticControls", true },
                                                     { "richInput", true },
                                                     { "screenshots", true },
                                                     { "tracing", true },
                                                     { "windows", true } }) },
                             { "security", object ({ { "allowInput", options.allowInput },
                                                     { "allowMutation", options.allowMutation },
                                                     { "allowFileWrite", options.allowFileWrite },
                                                     { "artifactRoot", options.artifactRoot.getFullPathName() } }) } });
        }

        juce::var windows() const
        {
            juce::Array<juce::var> result;

            if (root != nullptr)
            {
                result.add (object ({ { "id", "root" },
                                      { "title", componentString (root.getComponent()) },
                                      { "root", componentString (root.getComponent()) },
                                      { "focused", root->hasKeyboardFocus (true) },
                                      { "bounds", rectangleToVar (root->getScreenBounds()) } }));
            }

            return object ({ { "windows", result } });
        }

        juce::var traceStart (juce::DynamicObject& params)
        {
            auto requestedFile = getString (params, "file", {});

            if (requestedFile.isEmpty())
                requestedFile = "melatonin-automation-trace.json";

            auto fileOrError = writableArtifactFile (requestedFile);

            if (isError (fileOrError))
                return fileOrError;

            traceFile = juce::File (fileOrError.toString());
            traceEvents.clear();
            traceEnabled = true;

            return object ({ { "trace", traceFile.getFullPathName() } });
        }

        juce::var traceStop()
        {
            traceEnabled = false;

            if (traceFile.getFullPathName().isEmpty())
                return error ("trace_not_started", "Trace has not been started.");

            auto payload = object ({ { "events", traceEvents } });
            traceFile.getParentDirectory().createDirectory();

            if (!traceFile.replaceWithText (juce::JSON::toString (payload, true)))
                return error ("trace_write_failed", "Could not write trace file: " + traceFile.getFullPathName());

            return object ({ { "trace", traceFile.getFullPathName() },
                             { "events", traceEvents.size() } });
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
            const auto stateHash = calculateStateHash (tree);

            if (format == "json")
                return object ({ { "generation", generation }, { "stateHash", stateHash }, { "tree", tree }, { "text", text } });

            return object ({ { "generation", generation }, { "stateHash", stateHash }, { "text", text } });
        }

        juce::var locator (juce::DynamicObject& params)
        {
            auto matchesOrError = resolveLocatorQuery (params, false, false);

            if (isError (matchesOrError))
                return matchesOrError;

            return matchesOrError;
        }

        juce::var count (juce::DynamicObject& params)
        {
            auto matchesOrError = resolveLocatorQuery (params, false, false);

            if (isError (matchesOrError))
                return matchesOrError;

            auto* result = matchesOrError.getDynamicObject();
            return object ({ { "count", result != nullptr ? (int) result->getProperty ("count") : 0 } });
        }

        juce::var describe (juce::DynamicObject& params)
        {
            auto resolution = resolveTarget (params, false, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            juce::DynamicObject snapshotParams;
            snapshotParams.setProperty ("format", "json");
            snapshotParams.setProperty ("depth", 64);
            auto query = resolveLocatorQuery (params, false, true);

            if (isError (query))
                return query;

            auto* queryObject = query.getDynamicObject();
            auto matches = queryObject != nullptr ? queryObject->getProperty ("matches") : juce::var();

            if (matches.isArray() && !matches.getArray()->isEmpty())
                return object ({ { "match", matches.getArray()->getReference (0) } });

            return error ("locator_not_found", "Locator did not match any component.");
        }

        juce::var screenshot (juce::DynamicObject& params)
        {
            if (root == nullptr)
                return error ("no_root", "No root component is attached.");

            const auto ref = getString (params, "ref", {});
            juce::Component* target = nullptr;
            auto targetName = getString (params, "target", {});

            if (hasTargetSelector (params))
            {
                auto resolution = resolveTarget (params, false, true);

                if (!resolution.error.isVoid())
                    return resolution.error;

                target = resolution.component;
            }

            if (ref.isNotEmpty() && target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            if (!hasTargetSelector (params) && ref.isEmpty() && (targetName == "root" || target == nullptr))
                target = root.getComponent();

            if (target == nullptr)
                return error ("stale_ref", "Run snapshot again.");

            auto area = target->getLocalBounds();

            if (!params.getProperty ("clipW").isVoid() || !params.getProperty ("clipH").isVoid())
            {
                area = { getInt (params, "clipX", 0),
                         getInt (params, "clipY", 0),
                         getInt (params, "clipW", area.getWidth()),
                         getInt (params, "clipH", area.getHeight()) };
                area = area.getIntersection (target->getLocalBounds());
            }

            if (area.isEmpty())
                return error ("screenshot_failed", "Screenshot clip is empty.");

            auto image = target->createComponentSnapshot (area, false, (float) getDouble (params, "scale", 1.0));

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
                auto fileOrError = writableArtifactFile (filePath);

                if (auto* errorObject = fileOrError.getDynamicObject())
                    if (errorObject->getProperty ("__error").isString())
                        return fileOrError;

                auto file = juce::File (fileOrError.toString());

                file.getParentDirectory().createDirectory();

                if (!file.replaceWithData (pngBytes.getData(), pngBytes.getSize()))
                    return error ("screenshot_failed", "Could not write PNG file: " + file.getFullPathName());

                absolutePath = file.getFullPathName();
            }

            auto result = object ({ { "mimeType", "image/png" },
                                    { "width", image.getWidth() },
                                    { "height", image.getHeight() },
                                    { "file", absolutePath } });

            if (params.getProperty ("includeBase64").isVoid() || (bool) params.getProperty ("includeBase64"))
                result.getDynamicObject()->setProperty ("base64", juce::Base64::toBase64 (pngBytes.getData(), pngBytes.getSize()));

            return result;
        }

        juce::var writableArtifactFile (const juce::String& requestedPath) const
        {
            if (!options.allowFileWrite)
                return error ("file_write_disabled", "Automation file output is disabled for this session.");

            juce::File file (requestedPath);
            const auto hasArtifactRoot = options.artifactRoot.getFullPathName().isNotEmpty();

            if (hasArtifactRoot)
            {
                auto rootDirectory = options.artifactRoot;

                if (!juce::File::isAbsolutePath (requestedPath))
                    file = rootDirectory.getChildFile (requestedPath);

                if (!file.isAChildOf (rootDirectory))
                    return error ("artifact_path_denied", "Automation file output must stay within the artifact root.");
            }

            return file.getFullPathName();
        }

        juce::var click (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            activateComponent (*target);
            return snapshotAfterAction();
        }

        juce::var doubleClick (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            if (auto* button = dynamic_cast<juce::Button*> (target))
            {
                button->triggerClick();
                button->triggerClick();
            }
            else
            {
                synthesizeComponentClick (*target, juce::ModifierKeys(), 2);
            }

            return snapshotAfterAction();
        }

        juce::var rightClick (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            synthesizeComponentClick (*target, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier), 1);
            return snapshotAfterAction();
        }

        juce::var clickXY (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            const auto rootPoint = juce::Point<int> { getInt (params, "x", 0), getInt (params, "y", 0) };

            if (root != nullptr)
            {
                if (auto* target = findComponentAt (*root, rootPoint))
                {
                    if (target->isEnabled())
                    {
                        if (auto* button = dynamic_cast<juce::Button*> (target))
                            button->triggerClick();
                        else
                            synthesizeClickAt (rootPoint);
                    }
                }
                else
                {
                    synthesizeClickAt (rootPoint);
                }
            }

            return snapshotAfterAction();
        }

        juce::var mouseMove (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            sendPeerMouseEvent (pointFromParams (params, "x", "y"), juce::ModifierKeys(), 0.0f);
            return snapshotAfterAction();
        }

        juce::var mouseButton (juce::DynamicObject& params, bool isDown)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            sendPeerMouseEvent (pointFromParams (params, "x", "y"),
                                isDown ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys(),
                                isDown ? 1.0f : 0.0f);
            return snapshotAfterAction();
        }

        juce::var wheel (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            if (auto* peer = getRootPeer())
            {
                juce::MouseWheelDetails details;
                details.deltaX = (float) params.getProperty ("deltaX");
                details.deltaY = (float) params.getProperty ("deltaY");
                details.isReversed = (bool) params.getProperty ("isReversed");
                details.isSmooth = true;

                peer->handleMouseWheel (juce::MouseInputSource::InputSourceType::mouse,
                                        peer->getComponent().getLocalPoint (root.getComponent(), pointFromParams (params, "x", "y")).toFloat(),
                                        juce::Time::currentTimeMillis(),
                                        details);
            }

            return snapshotAfterAction();
        }

        juce::var dragXY (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            if (root == nullptr)
                return error ("no_root", "No root component is attached.");

            auto start = pointFromParams (params, "x", "y");
            auto end = pointFromParams (params, "toX", "toY");

            if (auto* target = findComponentAt (*root, start))
            {
                synthesizeDragOn (*target, start, end);
                return snapshotAfterAction();
            }

            return error ("locator_not_found", "No component was found at the drag start point.");
        }

        juce::var typeText (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

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

        juce::var fill (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            const auto text = getString (params, "text", {});

            if (auto* editor = dynamic_cast<juce::TextEditor*> (target))
            {
                editor->setText (text, juce::sendNotification);
                return snapshotAfterAction();
            }

            if (auto* label = dynamic_cast<juce::Label*> (target))
            {
                label->setText (text, juce::sendNotification);
                return snapshotAfterAction();
            }

            return error ("target_not_editable", "Target component does not support semantic fill.");
        }

        juce::var clear (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            if (auto* editor = dynamic_cast<juce::TextEditor*> (target))
            {
                editor->clear();
                return snapshotAfterAction();
            }

            if (auto* label = dynamic_cast<juce::Label*> (target))
            {
                label->setText ({}, juce::sendNotification);
                return snapshotAfterAction();
            }

            return error ("target_not_editable", "Target component does not support semantic clear.");
        }

        juce::var check (juce::DynamicObject& params, bool shouldBeChecked)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            if (auto* button = dynamic_cast<juce::Button*> (target))
            {
                if (!button->isToggleable())
                    return error ("target_not_toggleable", "Target button is not toggleable.");

                button->setToggleState (shouldBeChecked, juce::sendNotification);
                return snapshotAfterAction();
            }

            return error ("target_not_toggleable", "Target component does not support check/uncheck.");
        }

        juce::var setChecked (juce::DynamicObject& params)
        {
            if (params.getProperty ("checked").isVoid())
                return error ("invalid_checked_state", "set_checked requires a checked boolean.");

            return check (params, (bool) params.getProperty ("checked"));
        }

        juce::var setValue (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            if (auto* slider = dynamic_cast<juce::Slider*> (target))
            {
                slider->setValue ((double) params.getProperty ("value"), juce::sendNotificationSync);
                return snapshotAfterAction();
            }

            if (auto* editor = dynamic_cast<juce::TextEditor*> (target))
            {
                editor->setText (params.getProperty ("value").toString(), juce::sendNotification);
                return snapshotAfterAction();
            }

            return error ("target_no_value", "Target component does not support semantic set_value.");
        }

        juce::var selectOption (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            auto* combo = dynamic_cast<juce::ComboBox*> (target);

            auto text = getString (params, "text", {});

            if (combo == nullptr)
            {
                if (auto* listBox = dynamic_cast<juce::ListBox*> (target))
                    return selectListBoxRow (*listBox, params, text);

                return error ("target_not_selectable", "Target component is not a ComboBox or ListBox.");
            }

            if (text.isNotEmpty())
            {
                for (int i = 0; i < combo->getNumItems(); ++i)
                {
                    if (normalizeForLocator (combo->getItemText (i)) == normalizeForLocator (text))
                    {
                        combo->setSelectedItemIndex (i, juce::sendNotificationSync);
                        return snapshotAfterAction();
                    }
                }

                return error ("option_not_found", "ComboBox option not found: " + text);
            }

            if (!params.getProperty ("index").isVoid())
            {
                combo->setSelectedItemIndex ((int) params.getProperty ("index"), juce::sendNotificationSync);
                return snapshotAfterAction();
            }

            if (!params.getProperty ("id").isVoid())
            {
                combo->setSelectedId ((int) params.getProperty ("id"), juce::sendNotificationSync);
                return snapshotAfterAction();
            }

            return error ("invalid_option", "select_option requires text, index, or id.");
        }

        juce::var selectListBoxRow (juce::ListBox& listBox, juce::DynamicObject& params, const juce::String& text)
        {
            int row = -1;

            if (text.isNotEmpty())
            {
                if (auto* model = listBox.getListBoxModel())
                {
                    for (int i = 0; i < model->getNumRows(); ++i)
                    {
                        if (matchesString (model->getNameForRow (i), text, (bool) params.getProperty ("exact")))
                        {
                            row = i;
                            break;
                        }
                    }
                }
            }
            else if (!params.getProperty ("index").isVoid())
            {
                row = (int) params.getProperty ("index");
            }
            else if (!params.getProperty ("id").isVoid())
            {
                row = (int) params.getProperty ("id");
            }

            if (row < 0)
                return error ("option_not_found", "ListBox option not found: " + text);

            if (auto* model = listBox.getListBoxModel())
            {
                if (row >= model->getNumRows())
                    return error ("option_not_found", "ListBox row is out of range: " + juce::String (row));
            }

            listBox.selectRow (row, false, true);
            return snapshotAfterAction();
        }

        juce::var selectTab (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            auto* tabs = dynamic_cast<juce::TabbedComponent*> (target);

            if (tabs == nullptr)
                return error ("target_not_tabbed_component", "Target component is not a TabbedComponent.");

            if (!params.getProperty ("index").isVoid())
            {
                tabs->setCurrentTabIndex ((int) params.getProperty ("index"));
                return snapshotAfterAction();
            }

            auto name = getString (params, "name", {});
            auto tabNames = tabs->getTabNames();

            for (int i = 0; i < tabNames.size(); ++i)
            {
                if (normalizeForLocator (tabNames[i]) == normalizeForLocator (name))
                {
                    tabs->setCurrentTabIndex (i);
                    return snapshotAfterAction();
                }
            }

            return error ("tab_not_found", "TabbedComponent tab not found: " + name);
        }

        juce::var pressKey (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto key = parseKey (getString (params, "key", {}));
            juce::Component* target = nullptr;

            if (hasTargetSelector (params))
            {
                auto resolution = resolveTarget (params, true, true);

                if (!resolution.error.isVoid())
                    return resolution.error;

                target = resolution.component;
            }

            if (target != nullptr)
            {
                if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                    return validationError;

                if (isTrial (params))
                    return actionabilityResult (*target);

                target->grabKeyboardFocus();
            }

            if (target != nullptr && target->keyPressed (key.keyPress))
                return snapshotAfterAction();

            if (auto* peer = getRootPeer())
                peer->handleKeyPress (key.keyPress);

            return snapshotAfterAction();
        }

        juce::var keyDown (juce::DynamicObject& params)
        {
            return keyUpOrDown (params, true);
        }

        juce::var keyUp (juce::DynamicObject& params)
        {
            return keyUpOrDown (params, false);
        }

        juce::var keyUpOrDown (juce::DynamicObject& params, bool isDown)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto key = parseKey (getString (params, "key", {}));
            juce::Component* target = nullptr;

            if (hasTargetSelector (params))
            {
                auto resolution = resolveTarget (params, true, true);

                if (!resolution.error.isVoid())
                    return resolution.error;

                target = resolution.component;
            }

            if (target != nullptr)
            {
                if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                    return validationError;

                if (isTrial (params))
                    return actionabilityResult (*target);

                target->grabKeyboardFocus();

                if (isDown && target->keyPressed (key.keyPress))
                    return snapshotAfterAction();
            }

            if (auto* peer = getRootPeer())
            {
                peer->handleKeyUpOrDown (isDown);

                if (isDown)
                    peer->handleKeyPress (key.keyPress);
            }

            return snapshotAfterAction();
        }

        juce::var drag (juce::DynamicObject& params)
        {
            if (!options.allowInput)
                return error ("input_disabled", "Automation input is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

            if (auto validationError = validateInputTarget (*target, params); !validationError.isVoid())
                return validationError;

            if (isTrial (params))
                return actionabilityResult (*target);

            auto start = getRootBounds (*target).getCentre();
            auto end = start.translated (getInt (params, "dx", 0), getInt (params, "dy", 0));

            synthesizeDragOn (*target, start, end);

            return snapshotAfterAction();
        }

        juce::var setBounds (juce::DynamicObject& params)
        {
            if (!options.allowMutation)
                return error ("mutation_disabled", "Automation mutation is disabled for this session.");

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

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

            auto resolution = resolveTarget (params, true, true);

            if (!resolution.error.isVoid())
                return resolution.error;

            auto* target = resolution.component;

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

        bool hasTargetSelector (juce::DynamicObject& params) const
        {
            return getString (params, "ref", {}).isNotEmpty() || params.getProperty ("locator").isObject();
        }

        TargetResolution resolveTarget (juce::DynamicObject& params, bool defaultVisible, bool requireStrict)
        {
            const auto ref = getString (params, "ref", {});
            auto locatorValue = params.getProperty ("locator");
            auto* locatorObject = locatorValue.getDynamicObject();

            if (ref.isNotEmpty() && locatorObject != nullptr)
                return { nullptr, error ("invalid_locator", "Pass either ref or locator, not both.") };

            if (ref.isNotEmpty())
            {
                auto* target = getTargetComponent (ref);
                return { target, target != nullptr ? juce::var() : error ("stale_ref", "Run snapshot again.") };
            }

            if (locatorObject == nullptr)
                return { nullptr, error ("stale_ref", "Run snapshot again.") };

            auto result = resolveLocatorQuery (*locatorObject, defaultVisible, requireStrict);

            if (isError (result))
                return { nullptr, result };

            auto* resultObject = result.getDynamicObject();
            auto matches = resultObject != nullptr ? resultObject->getProperty ("matches") : juce::var();

            if (!matches.isArray() || matches.getArray()->isEmpty())
                return { nullptr, error ("locator_not_found", "Locator did not match any component.") };

            auto refValue = asObjectProperty (matches.getArray()->getReference (0), "ref");
            auto* target = getTargetComponent (refValue);

            return { target, target != nullptr ? juce::var() : error ("stale_ref", "Run snapshot again.") };
        }

        juce::DynamicObject* getLocatorObject (juce::DynamicObject& params) const
        {
            auto locatorValue = params.getProperty ("locator");

            if (auto* locatorObject = locatorValue.getDynamicObject())
                return locatorObject;

            for (auto name : { "role",
                               "name",
                               "text",
                               "componentId",
                               "componentName",
                               "testId",
                               "class",
                               "value",
                               "hasText",
                               "nth",
                               "exact",
                               "visible",
                               "enabled",
                               "focused" })
            {
                if (!params.getProperty (name).isVoid())
                    return &params;
            }

            return nullptr;
        }

        juce::var resolveLocatorQuery (juce::DynamicObject& params, bool defaultVisible, bool requireStrict)
        {
            if (root == nullptr)
                return error ("no_root", "No root component is attached.");

            auto* locatorObject = getLocatorObject (params);

            if (locatorObject == nullptr)
                return error ("invalid_locator", "Locator must contain at least one field.");

            refs.clear();
            ++generation;

            auto tree = serializeComponent (*root, 0, 64);
            juce::Array<juce::var> matches;
            collectLocatorMatches (matches, tree, *locatorObject, defaultVisible);

            const auto nthValue = locatorObject->getProperty ("nth");

            if (!nthValue.isVoid())
            {
                const auto nth = (int) nthValue;

                if (juce::isPositiveAndBelow (nth, matches.size()))
                {
                    auto selected = matches[nth];
                    matches.clear();
                    matches.add (selected);
                }
                else
                {
                    matches.clear();
                }
            }

            if (requireStrict)
            {
                if (matches.isEmpty())
                    return error ("locator_not_found", "Locator did not match any component.");

                if (matches.size() > 1)
                    return error ("strict_mode_violation", "Locator matched " + juce::String (matches.size()) + " components: " + summarizeMatches (matches));
            }

            return object ({ { "generation", generation },
                             { "stateHash", calculateStateHash (tree) },
                             { "count", matches.size() },
                             { "matches", matches } });
        }

        void collectLocatorMatches (juce::Array<juce::var>& matches, const juce::var& node, juce::DynamicObject& locatorObject, bool defaultVisible) const
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return;

            if (matchesLocator (*object, locatorObject, defaultVisible))
                matches.add (summarizeNode (*object));

            auto children = object->getProperty ("children");

            if (children.isArray())
                for (auto& child : *children.getArray())
                    collectLocatorMatches (matches, child, locatorObject, defaultVisible);
        }

        bool matchesLocator (juce::DynamicObject& node, juce::DynamicObject& locatorObject, bool defaultVisible) const
        {
            const auto exact = (bool) locatorObject.getProperty ("exact");

            if (!matchesOptionalString (node, locatorObject, "role", "role", exact)) return false;
            if (!matchesOptionalText (searchableName (node), locatorObject, "name", exact)) return false;
            if (!matchesOptionalText (searchableText (node), locatorObject, "text", exact)) return false;
            if (!matchesOptionalString (node, locatorObject, "componentId", "componentId", true)) return false;
            if (!matchesOptionalString (node, locatorObject, "componentId", "testId", true)) return false;
            if (!matchesOptionalString (node, locatorObject, "componentName", "componentName", true)) return false;
            if (!matchesOptionalString (node, locatorObject, "class", "class", exact)) return false;
            if (!matchesOptionalString (node, locatorObject, "value", "value", exact)) return false;
            if (!matchesOptionalText (searchableText (node), locatorObject, "hasText", exact)) return false;

            if (!matchesOptionalBool (node, locatorObject, "enabled", "enabled")) return false;
            if (!matchesOptionalBool (node, locatorObject, "focused", "focused")) return false;

            if (!locatorObject.getProperty ("visible").isVoid())
            {
                if (!matchesOptionalBool (node, locatorObject, "visible", "visible"))
                    return false;
            }
            else if (defaultVisible && !(bool) node.getProperty ("visible"))
            {
                return false;
            }

            return true;
        }

        static bool matchesOptionalString (juce::DynamicObject& node,
                                           juce::DynamicObject& locatorObject,
                                           const juce::Identifier& nodeProperty,
                                           const juce::Identifier& locatorProperty,
                                           bool exact)
        {
            auto expected = locatorObject.getProperty (locatorProperty);

            if (expected.isVoid())
                return true;

            auto actual = node.getProperty (nodeProperty).toString();
            return matchesString (actual, expected.toString(), exact);
        }

        static bool matchesOptionalText (const juce::String& actual, juce::DynamicObject& locatorObject, const juce::Identifier& locatorProperty, bool exact)
        {
            auto expected = locatorObject.getProperty (locatorProperty);

            if (expected.isVoid())
                return true;

            return matchesString (actual, expected.toString(), exact);
        }

        static bool matchesOptionalBool (juce::DynamicObject& node,
                                         juce::DynamicObject& locatorObject,
                                         const juce::Identifier& nodeProperty,
                                         const juce::Identifier& locatorProperty)
        {
            auto expected = locatorObject.getProperty (locatorProperty);

            if (expected.isVoid())
                return true;

            return (bool) node.getProperty (nodeProperty) == (bool) expected;
        }

        static bool matchesString (const juce::String& actual, const juce::String& expected, bool exact)
        {
            const auto normalizedActual = normalizeForLocator (actual);
            const auto normalizedExpected = normalizeForLocator (expected);

            return exact ? normalizedActual == normalizedExpected
                         : normalizedActual.contains (normalizedExpected);
        }

        static juce::String normalizeForLocator (juce::String text)
        {
            return text.replaceCharacter ('\n', ' ')
                       .replaceCharacter ('\t', ' ')
                       .trim()
                       .replace ("  ", " ")
                       .toLowerCase();
        }

        static juce::String searchableName (juce::DynamicObject& node)
        {
            return node.getProperty ("title").toString().isNotEmpty()
                       ? node.getProperty ("title").toString()
                       : node.getProperty ("name").toString();
        }

        static juce::String searchableText (juce::DynamicObject& node)
        {
            return node.getProperty ("name").toString() + " "
                   + node.getProperty ("title").toString() + " "
                   + node.getProperty ("value").toString();
        }

        static bool treeContainsText (const juce::var& node, const juce::String& expected, bool exact, const juce::var& visible)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return false;

            const auto nodeVisible = (bool) object->getProperty ("visible");
            const auto visibilityMatches = visible.isVoid() ? nodeVisible
                                                            : nodeVisible == (bool) visible;

            if (visibilityMatches && matchesString (searchableText (*object), expected, exact))
                return true;

            auto children = object->getProperty ("children");

            if (children.isArray())
                for (const auto& child : *children.getArray())
                    if (treeContainsText (child, expected, exact, visible))
                        return true;

            return false;
        }

        static juce::String semanticValueFor (juce::Component& component)
        {
            if (auto* slider = dynamic_cast<juce::Slider*> (&component))
                return juce::String (slider->getValue());

            if (auto* combo = dynamic_cast<juce::ComboBox*> (&component))
                return combo->getText();

            if (auto* editor = dynamic_cast<juce::TextEditor*> (&component))
                return editor->getText();

            if (auto* label = dynamic_cast<juce::Label*> (&component))
                return label->getText();

            if (auto* button = dynamic_cast<juce::Button*> (&component))
                return button->getToggleState() ? "true" : "false";

            if (component.isAccessible() && component.getAccessibilityHandler() != nullptr)
                if (auto* valueInterface = component.getAccessibilityHandler()->getValueInterface())
                    return valueInterface->getCurrentValueAsString();

            return {};
        }

        static juce::var summarizeNode (juce::DynamicObject& node)
        {
            return object ({ { "ref", node.getProperty ("ref") },
                             { "name", node.getProperty ("name") },
                             { "componentId", node.getProperty ("componentId") },
                             { "componentName", node.getProperty ("componentName") },
                             { "class", node.getProperty ("class") },
                             { "role", node.getProperty ("role") },
                             { "value", node.getProperty ("value") },
                             { "visible", node.getProperty ("visible") },
                             { "enabled", node.getProperty ("enabled") },
                             { "focused", node.getProperty ("focused") },
                             { "bounds", node.getProperty ("bounds") } });
        }

        static juce::String summarizeMatches (const juce::Array<juce::var>& matches)
        {
            juce::StringArray lines;

            for (auto& match : matches)
            {
                if (auto* object = match.getDynamicObject())
                    lines.add (object->getProperty ("ref").toString()
                               + " " + object->getProperty ("class").toString()
                               + " \"" + object->getProperty ("name").toString() + "\"");
            }

            return lines.joinIntoString ("; ");
        }

        static bool isError (const juce::var& value)
        {
            if (auto* object = value.getDynamicObject())
                return object->getProperty ("__error").isString();

            return false;
        }

        void recordTraceEvent (const juce::String& method, const juce::var& result)
        {
            if (!traceEnabled || method == "trace_start" || method == "trace_stop")
                return;

            juce::String errorCode;

            if (auto* object = result.getDynamicObject())
                errorCode = object->getProperty ("__error").toString();

            traceEvents.add (object ({ { "timeMs", (double) juce::Time::currentTimeMillis() },
                                       { "method", method },
                                       { "ok", errorCode.isEmpty() },
                                       { "error", errorCode } }));
        }

        static juce::String asObjectProperty (const juce::var& value, const juce::Identifier& property)
        {
            if (auto* object = value.getDynamicObject())
                return object->getProperty (property).toString();

            return {};
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

        void activateComponent (juce::Component& target)
        {
            if (auto* button = dynamic_cast<juce::Button*> (&target))
            {
                button->triggerClick();
                return;
            }

            synthesizeComponentClick (target, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1);
        }

        juce::var validateInputTarget (juce::Component& target, juce::DynamicObject& params) const
        {
            if ((bool) params.getProperty ("force"))
                return {};

            if (!target.isShowing())
                return error ("target_not_showing", "Target component is not showing.");

            if (!target.isEnabled())
                return error ("target_disabled", "Target component is disabled.");

            if (getRootBounds (target).isEmpty())
                return error ("target_empty_bounds", "Target component has empty bounds.");

            if (!receivesEvents (target))
                return error ("target_not_receiving_events", "Target component does not receive pointer events at its center.");

            return {};
        }

        static bool isTrial (juce::DynamicObject& params)
        {
            return (bool) params.getProperty ("trial");
        }

        juce::var actionabilityResult (juce::Component& target) const
        {
            const auto bounds = getRootBounds (target);
            return object ({ { "actionability", object ({ { "attached", true },
                                                          { "visible", target.isShowing() && !bounds.isEmpty() },
                                                          { "enabled", target.isEnabled() },
                                                          { "nonEmptyBounds", !bounds.isEmpty() },
                                                          { "receivesEvents", receivesEvents (target) } }) } });
        }

        bool receivesEvents (juce::Component& target) const
        {
            if (root == nullptr)
                return false;

            auto rootBounds = getRootBounds (target);

            if (rootBounds.isEmpty())
                return false;

            auto* found = findComponentAt (*root, rootBounds.getCentre());

            return found == &target || (found != nullptr && target.isParentOf (found));
        }

        void synthesizeComponentClick (juce::Component& target, juce::ModifierKeys buttonModifiers, int numberOfClicks)
        {
            if (root == nullptr)
                return;

            auto rootPoint = getRootBounds (target).getCentre();
            auto localPoint = target.getLocalPoint (root, rootPoint).toFloat();
            auto source = juce::Desktop::getInstance().getMainMouseSource();
            auto now = juce::Time::getCurrentTime();

            target.mouseDown ({ source,
                                localPoint,
                                buttonModifiers,
                                1.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                &target,
                                &target,
                                now,
                                localPoint,
                                now,
                                numberOfClicks,
                                false });

            target.mouseUp ({ source,
                              localPoint,
                              juce::ModifierKeys(),
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              &target,
                              &target,
                              now + juce::RelativeTime::milliseconds (2),
                              localPoint,
                              now,
                              numberOfClicks,
                              false });

            if (numberOfClicks >= 2)
            {
                target.mouseDoubleClick ({ source,
                                           localPoint,
                                           buttonModifiers,
                                           1.0f,
                                           0.0f,
                                           0.0f,
                                           0.0f,
                                           0.0f,
                                           &target,
                                           &target,
                                           now + juce::RelativeTime::milliseconds (3),
                                           localPoint,
                                           now,
                                           numberOfClicks,
                                           false });
            }
        }

        void synthesizeDragOn (juce::Component& target, juce::Point<int> rootStart, juce::Point<int> rootEnd)
        {
            if (root == nullptr)
                return;

            auto start = target.getLocalPoint (root, rootStart).toFloat();
            auto end = target.getLocalPoint (root, rootEnd).toFloat();
            auto source = juce::Desktop::getInstance().getMainMouseSource();
            auto now = juce::Time::getCurrentTime();
            auto downModifiers = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);

            target.mouseDown ({ source,
                                start,
                                downModifiers,
                                1.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                &target,
                                &target,
                                now,
                                start,
                                now,
                                1,
                                false });

            target.mouseDrag ({ source,
                                end,
                                downModifiers,
                                1.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                0.0f,
                                &target,
                                &target,
                                now + juce::RelativeTime::milliseconds (16),
                                start,
                                now,
                                1,
                                true });

            target.mouseUp ({ source,
                              end,
                              juce::ModifierKeys(),
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              &target,
                              &target,
                              now + juce::RelativeTime::milliseconds (17),
                              start,
                              now,
                              1,
                              true });
        }

        juce::Component* findComponentAt (juce::Component& component, juce::Point<int> localPoint) const
        {
            for (int i = component.getNumChildComponents(); --i >= 0;)
            {
                auto* child = component.getChildComponent (i);

                if (child == nullptr || !child->isVisible() || isInspectorInternalComponent (*child))
                    continue;

                if (!child->getBounds().contains (localPoint))
                    continue;

                if (auto* found = findComponentAt (*child, child->getLocalPoint (&component, localPoint)))
                    return found;
            }

            return component.getLocalBounds().contains (localPoint) ? &component : nullptr;
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

        void sendPeerMouseEvent (juce::Point<int> rootPoint, juce::ModifierKeys modifiers, float pressure)
        {
            if (root == nullptr)
                return;

            if (auto* peer = getRootPeer())
            {
                auto peerPoint = peer->getComponent().getLocalPoint (root, rootPoint).toFloat();
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse,
                                        peerPoint,
                                        modifiers,
                                        pressure,
                                        0.0f,
                                        juce::Time::currentTimeMillis());
            }
        }

        static juce::Point<int> pointFromParams (juce::DynamicObject& params, const juce::Identifier& xName, const juce::Identifier& yName)
        {
            return { getInt (params, xName, 0), getInt (params, yName, 0) };
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
            auto ref = "m" + juce::String (generation) + "-" + juce::String (refs.size() + 1);
            refs.add ({ ref, &component });

            node->setProperty ("ref", ref);
            node->setProperty ("name", componentString (&component));
            node->setProperty ("componentId", component.getComponentID());
            node->setProperty ("componentName", component.getName());
            node->setProperty ("class", type (component));
            node->setProperty ("enabled", component.isEnabled());
            const auto bounds = getRootBounds (component);
            node->setProperty ("visible", component.isShowing() && !bounds.isEmpty());
            node->setProperty ("focused", component.hasKeyboardFocus (false));
            node->setProperty ("bounds", rectangleToVar (bounds));
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
                node->setProperty ("checked", button->getToggleState());
            }

            if (auto* editor = dynamic_cast<juce::TextEditor*> (&component))
            {
                node->setProperty ("editable", ! editor->isReadOnly());
                node->setProperty ("readOnly", editor->isReadOnly());
                node->setProperty ("value", editor->getText());
            }

            if (auto* label = dynamic_cast<juce::Label*> (&component))
            {
                node->setProperty ("editable", label->isEditable());
                node->setProperty ("readOnly", ! label->isEditable());
                node->setProperty ("value", label->getText());
            }

            if (auto* slider = dynamic_cast<juce::Slider*> (&component))
            {
                node->setProperty ("value", slider->getValue());
                node->setProperty ("minimum", slider->getMinimum());
                node->setProperty ("maximum", slider->getMaximum());
                node->setProperty ("interval", slider->getInterval());
            }

            if (auto* combo = dynamic_cast<juce::ComboBox*> (&component))
            {
                node->setProperty ("value", combo->getText());
                node->setProperty ("selectedIndex", combo->getSelectedItemIndex());
                node->setProperty ("selectedId", combo->getSelectedId());
                node->setProperty ("selectedText", combo->getText());
                node->setProperty ("options", comboOptionsToVar (*combo));
            }

            if (auto* tabs = dynamic_cast<juce::TabbedComponent*> (&component))
            {
                auto tabNames = tabs->getTabNames();
                const auto currentTabIndex = tabs->getCurrentTabIndex();

                node->setProperty ("tabNames", stringArrayToVar (tabNames));
                node->setProperty ("currentTabIndex", currentTabIndex);

                if (juce::isPositiveAndBelow (currentTabIndex, tabNames.size()))
                    node->setProperty ("currentTab", tabNames[currentTabIndex]);
            }

            if (auto* viewport = dynamic_cast<juce::Viewport*> (&component))
            {
                node->setProperty ("scrollX", viewport->getViewPositionX());
                node->setProperty ("scrollY", viewport->getViewPositionY());
                node->setProperty ("viewWidth", viewport->getViewWidth());
                node->setProperty ("viewHeight", viewport->getViewHeight());

                if (auto* viewed = viewport->getViewedComponent())
                {
                    node->setProperty ("contentWidth", viewed->getWidth());
                    node->setProperty ("contentHeight", viewed->getHeight());
                }
            }

            if (auto* listBox = dynamic_cast<juce::ListBox*> (&component))
            {
                node->setProperty ("rowCount", listBox->getListBoxModel() != nullptr ? listBox->getListBoxModel()->getNumRows() : 0);
                node->setProperty ("selectedRow", listBox->getSelectedRow());
                node->setProperty ("selectedRows", selectedRowsToVar (*listBox));
                node->setProperty ("selectedText", listRowName (*listBox, listBox->getSelectedRow()));
                node->setProperty ("options", listRowsToVar (*listBox));
            }

            juce::Array<juce::var> children;

            if (depth < maxDepth)
                addSerializedChildren (children, component, depth + 1, maxDepth);

            node->setProperty ("children", children);
            return node;
        }

        static juce::var stringArrayToVar (const juce::StringArray& values)
        {
            juce::Array<juce::var> result;

            for (const auto& value : values)
                result.add (value);

            return result;
        }

        static juce::var comboOptionsToVar (juce::ComboBox& combo)
        {
            juce::Array<juce::var> result;

            for (int i = 0; i < combo.getNumItems(); ++i)
                result.add (object ({ { "index", i },
                                      { "id", combo.getItemId (i) },
                                      { "text", combo.getItemText (i) } }));

            return result;
        }

        static juce::String listRowName (juce::ListBox& listBox, int row)
        {
            auto* model = listBox.getListBoxModel();

            if (model == nullptr || ! juce::isPositiveAndBelow (row, model->getNumRows()))
                return {};

            return model->getNameForRow (row);
        }

        static juce::var listRowsToVar (juce::ListBox& listBox)
        {
            juce::Array<juce::var> result;
            auto* model = listBox.getListBoxModel();

            if (model == nullptr)
                return result;

            const auto rowsToExpose = juce::jmin (model->getNumRows(), 200);

            for (int i = 0; i < rowsToExpose; ++i)
                result.add (object ({ { "index", i }, { "text", model->getNameForRow (i) } }));

            return result;
        }

        static juce::var selectedRowsToVar (juce::ListBox& listBox)
        {
            juce::Array<juce::var> result;
            auto selectedRows = listBox.getSelectedRows();

            for (int i = 0; i < selectedRows.size(); ++i)
                result.add (selectedRows[i]);

            return result;
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

        static juce::String calculateStateHash (const juce::var& tree)
        {
            const auto canonical = canonicalizeSnapshotNode (tree);
            return juce::String::toHexString (juce::JSON::toString (canonical, true).hashCode64());
        }

        static juce::var canonicalizeSnapshotNode (const juce::var& node)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return {};

            auto* result = new juce::DynamicObject();

            for (auto name : { "name",
                               "componentId",
                               "componentName",
                               "class",
                               "enabled",
                               "visible",
                               "focused",
                               "role",
                               "title",
                               "value",
                               "toggleable",
                               "toggleState",
                               "checked",
                               "editable",
                               "readOnly",
                               "selectedIndex",
                               "selectedId",
                               "selectedText",
                               "minimum",
                               "maximum",
                               "interval",
                               "tabNames",
                               "currentTabIndex",
                               "currentTab",
                               "scrollX",
                               "scrollY",
                               "viewWidth",
                               "viewHeight",
                               "contentWidth",
                               "contentHeight",
                               "rowCount",
                               "selectedRow",
                               "selectedRows",
                               "options" })
            {
                auto property = object->getProperty (name);

                if (!property.isVoid())
                    result->setProperty (name, property);
            }

            result->setProperty ("bounds", object->getProperty ("bounds"));

            juce::Array<juce::var> children;
            auto sourceChildren = object->getProperty ("children");

            if (sourceChildren.isArray())
                for (auto& child : *sourceChildren.getArray())
                    children.add (canonicalizeSnapshotNode (child));

            result->setProperty ("children", children);
            return result;
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

        struct ParsedKey
        {
            juce::KeyPress keyPress;
        };

        static ParsedKey parseKey (const juce::String& key)
        {
            juce::StringArray tokens;
            tokens.addTokens (key, "+", {});
            tokens.trim();
            tokens.removeEmptyStrings();

            juce::ModifierKeys modifiers;
            auto keyName = tokens.isEmpty() ? key : tokens[tokens.size() - 1];

            for (int i = 0; i < tokens.size() - 1; ++i)
            {
                auto modifier = tokens[i].trim().toLowerCase();

                if (modifier == "shift")
                    modifiers = modifiers.withFlags (juce::ModifierKeys::shiftModifier);
                else if (modifier == "control" || modifier == "ctrl")
                    modifiers = modifiers.withFlags (juce::ModifierKeys::ctrlModifier);
                else if (modifier == "alt" || modifier == "option")
                    modifiers = modifiers.withFlags (juce::ModifierKeys::altModifier);
                else if (modifier == "meta" || modifier == "cmd" || modifier == "command")
                    modifiers = modifiers.withFlags (juce::ModifierKeys::commandModifier);
            }

            const auto keyCode = keyCodeForName (keyName);
            const auto textCharacter = keyName.length() == 1 ? keyName[0] : juce::juce_wchar();
            return { juce::KeyPress (keyCode, modifiers, textCharacter) };
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
                explicit Call (std::function<juce::var()> fn)
                    : function (std::move (fn))
                {
                }

                std::function<juce::var()> function;
                juce::var result;
                juce::WaitableEvent completed;
                std::atomic<bool> cancelled { false };
            };

            auto call = std::make_shared<Call> (std::move (function));

            if (!juce::MessageManager::callAsync ([call] {
                    if (!call->cancelled.load())
                        call->result = call->function();

                    call->completed.signal();
                }))
            {
                return error ("message_thread_unavailable", "Could not post automation request to the JUCE message thread.");
            }

            while (!threadShouldExit())
                if (call->completed.wait (25))
                    return call->result;

            call->cancelled = true;
            return error ("shutting_down", "Automation endpoint is shutting down.");
        }

        void sleepUntilReadyOrStopped (int milliseconds)
        {
            auto remaining = milliseconds;

            while (remaining > 0 && !threadShouldExit())
            {
                const auto chunk = juce::jmin (remaining, 25);
                juce::Thread::sleep (chunk);
                remaining -= chunk;
            }
        }

        void waitForThreadToStop()
        {
            waitForThreadToExit (-1);
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

        static double getDouble (juce::DynamicObject& object, const juce::Identifier& name, double fallback)
        {
            auto value = object.getProperty (name);
            return value.isVoid() ? fallback : (double) value;
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
            restrictFilePermissions (directory, 0700);
            return directory;
        }

        static void restrictFilePermissions (const juce::File& file, int permissions)
        {
#if JUCE_WINDOWS
            juce::ignoreUnused (file, permissions);
#else
            ::chmod (file.getFullPathName().toRawUTF8(), (mode_t) permissions);
#endif
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
            restrictFilePermissions (advertisementFile, 0600);
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
