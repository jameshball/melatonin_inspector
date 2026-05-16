#include <juce_core/juce_core.h>
#include <cstdlib>
#include <iostream>
#include <string>

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

    juce::String readLine (juce::StreamingSocket& socket)
    {
        std::string bytes;
        char buffer[1024] {};

        for (;;)
        {
            const auto ready = socket.waitUntilReady (true, 5000);

            if (ready <= 0)
                break;

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

    juce::var request (juce::DynamicObject& session, const juce::String& method, juce::var params)
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

        auto response = juce::JSON::parse (readLine (socket));
        auto* responseObject = response.getDynamicObject();

        if (responseObject == nullptr)
            throw std::runtime_error ("Invalid response from automation endpoint");

        if (! (bool) responseObject->getProperty ("ok"))
        {
            auto* error = responseObject->getProperty ("error").getDynamicObject();
            auto message = error != nullptr ? error->getProperty ("message").toString() : "Unknown automation error";
            throw std::runtime_error (message.toStdString());
        }

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
        auto* locator = new juce::DynamicObject();
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

        return hasLocator ? juce::var (locator) : juce::var();
    }

    void addLocatorIfPresent (juce::DynamicObject& params, const juce::var& locator)
    {
        if (!locator.isVoid())
            params.setProperty ("locator", locator);
    }

    juce::var object (std::initializer_list<std::pair<juce::String, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();

        for (const auto& property : properties)
            result->setProperty (property.first, property.second);

        return result;
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

    juce::var booleanSchema()
    {
        return object ({ { "type", "boolean" } });
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
                                                   { "enabled", booleanSchema() },
                                                   { "focused", booleanSchema() },
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
            tool ("juce_capabilities",
                  "Return protocol, feature, and security capabilities for a running automation session.",
                  toolSchema ({ { "session", stringSchema() } })),
            tool ("juce_locator",
                  "Find JUCE components by Playwright-style locator fields.",
                  toolSchema ({ { "session", stringSchema() }, { "locator", locatorSchema() } }, { "locator" })),
            tool ("juce_snapshot",
                  "Return a compact Playwright-style snapshot of a JUCE component tree.",
                  toolSchema ({ { "session", stringSchema() },
                                { "format", object ({ { "type", "string" },
                                                       { "enum", array ({ "text", "json" }) },
                                                       { "default", "text" } }) },
                                { "depth", object ({ { "type", "number" }, { "default", 8 } }) } })),
            tool ("juce_screenshot",
                  "Capture a PNG screenshot of the root or a component ref.",
                  toolSchema ({ { "session", stringSchema() },
                                { "target", object ({ { "type", "string" }, { "default", "root" } }) },
                                { "ref", stringSchema() },
                                { "file", stringSchema() } })),
            tool ("juce_click",
                  "Click a component ref and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() } })),
            tool ("juce_click_xy",
                  "Click root-local coordinates and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "x", numberSchema() }, { "y", numberSchema() } }, { "x", "y" })),
            tool ("juce_type",
                  "Type text into a component ref and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "text", stringSchema() } }, { "text" })),
            tool ("juce_press",
                  "Press a key and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "key", stringSchema() } }, { "key" })),
            tool ("juce_drag",
                  "Drag a component by a delta and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "dx", numberSchema() }, { "dy", numberSchema() } })),
            tool ("juce_set_bounds",
                  "Set a component's bounds and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() },
                                { "ref", stringSchema() },
                                { "locator", locatorSchema() },
                                { "x", numberSchema() },
                                { "y", numberSchema() },
                                { "w", numberSchema() },
                                { "h", numberSchema() } })),
            tool ("juce_set_property",
                  "Set a component property and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ref", stringSchema() }, { "locator", locatorSchema() }, { "name", stringSchema() }, { "value", emptyObject() } },
                              { "name" })),
            tool ("juce_wait",
                  "Wait briefly and return a fresh snapshot.",
                  toolSchema ({ { "session", stringSchema() }, { "ms", object ({ { "type", "number" }, { "default", 250 } }) } }))
        });
    }

    juce::String methodForTool (const juce::String& name)
    {
        if (name == "juce_snapshot") return "snapshot";
        if (name == "juce_capabilities") return "capabilities";
        if (name == "juce_locator") return "locator";
        if (name == "juce_screenshot") return "screenshot";
        if (name == "juce_click") return "click";
        if (name == "juce_click_xy") return "click_xy";
        if (name == "juce_type") return "type";
        if (name == "juce_press") return "press";
        if (name == "juce_drag") return "drag";
        if (name == "juce_set_bounds") return "set_bounds";
        if (name == "juce_set_property") return "set_property";
        if (name == "juce_wait") return "wait";

        return {};
    }

    juce::var mcpTextContent (const juce::String& text)
    {
        return object ({ { "content", array ({ object ({ { "type", "text" }, { "text", text } }) }) } });
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

        auto result = request (*sessionObject, method, arguments);

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

        if (name == "juce_locator")
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
                    return;
                }

                auto file = object->getProperty ("file").toString();

                if (file.isNotEmpty())
                {
                    std::cout << file.toStdString() << "\n";
                    return;
                }
            }
        }

        std::cout << juce::JSON::toString (result, true).toStdString() << "\n";
    }

    void printHelp()
    {
        std::cout
            << "Usage:\n"
            << "  melatonin-ui list\n"
            << "  melatonin-ui mcp\n"
            << "  melatonin-ui -s <session> capabilities\n"
            << "  melatonin-ui -s <session> locator [--role role] [--name text] [--text text] [--format json]\n"
            << "  melatonin-ui -s <session> snapshot [--format text|json] [--depth n]\n"
            << "  melatonin-ui -s <session> screenshot [--target root|--ref m1-1] --file /tmp/root.png\n"
            << "  melatonin-ui -s <session> click <ref>\n"
            << "  melatonin-ui -s <session> click-xy <x> <y>\n"
            << "  melatonin-ui -s <session> type <ref> <text>\n"
            << "  melatonin-ui -s <session> press <key> [--ref m1]\n"
            << "  melatonin-ui -s <session> drag <ref> --dx n --dy n\n"
            << "  melatonin-ui -s <session> set-bounds <ref> --x n --y n --w n --h n\n"
            << "  melatonin-ui -s <session> set-property <ref> <name> <value>\n"
            << "  melatonin-ui -s <session> wait --ms n\n";
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
            auto format = optionValue (args, "--format", "text");
            auto depth = optionValue (args, "--depth", "8").getIntValue();
            auto result = request (*sessionObject, "snapshot", object ({ { "format", format }, { "depth", depth } }));
            printResult (result, format == "json");
            return 0;
        }

        if (command == "locator")
        {
            auto format = optionValue (args, "--format", "json");
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "locator", locator } });
            printResult (request (*sessionObject, "locator", params), format == "json");
            return 0;
        }

        if (command == "capabilities")
        {
            printResult (request (*sessionObject, "capabilities", emptyObject()), true);
            return 0;
        }

        if (command == "screenshot")
        {
            auto file = optionValue (args, "--file");
            auto ref = optionValue (args, "--ref");
            auto target = optionValue (args, "--target", "root");
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "file", file }, { "ref", ref }, { "target", target } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            auto result = request (*sessionObject, "screenshot", params);
            printResult (result);
            return 0;
        }

        if (command == "click")
        {
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "ref", args.size() >= 1 ? args[0] : juce::String() } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "click", params));
            return 0;
        }

        if (command == "click-xy" && args.size() >= 2)
        {
            printResult (request (*sessionObject, "click_xy", object ({ { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() } })));
            return 0;
        }

        if (command == "type")
        {
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);
            auto params = object ({ { "ref", ref }, { "text", args.joinIntoString (" ") } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "type", params));
            return 0;
        }

        if (command == "press" && args.size() >= 1)
        {
            auto ref = optionValue (args, "--ref");
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "key", args[0] }, { "ref", ref } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "press", params));
            return 0;
        }

        if (command == "drag")
        {
            auto dx = optionValue (args, "--dx", "0").getIntValue();
            auto dy = optionValue (args, "--dy", "0").getIntValue();
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "ref", args.size() >= 1 ? args[0] : juce::String() }, { "dx", dx }, { "dy", dy } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "drag", params));
            return 0;
        }

        if (command == "set-bounds")
        {
            auto x = optionValue (args, "--x", "0").getIntValue();
            auto y = optionValue (args, "--y", "0").getIntValue();
            auto w = optionValue (args, "--w", "0").getIntValue();
            auto h = optionValue (args, "--h", "0").getIntValue();
            auto locator = parseLocatorOptions (args);
            auto params = object ({ { "ref", args.size() >= 1 ? args[0] : juce::String() }, { "x", x }, { "y", y }, { "w", w }, { "h", h } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "set_bounds", params));
            return 0;
        }

        if (command == "set-property")
        {
            auto locator = parseLocatorOptions (args);
            auto ref = !locator.isVoid() ? juce::String() : popFront (args);

            if (args.size() < 2)
                throw std::runtime_error ("set-property requires a property name and value");

            auto params = object ({ { "ref", ref }, { "name", args[0] }, { "value", parseValue (args[1]) } });
            addLocatorIfPresent (*params.getDynamicObject(), locator);
            printResult (request (*sessionObject, "set_property", params));
            return 0;
        }

        if (command == "wait")
        {
            printResult (request (*sessionObject, "wait", object ({ { "ms", optionValue (args, "--ms", "250").getIntValue() } })));
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
