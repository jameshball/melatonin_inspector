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

    juce::var object (std::initializer_list<std::pair<juce::String, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();

        for (const auto& property : properties)
            result->setProperty (property.first, property.second);

        return result;
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

        if (command == "screenshot")
        {
            auto file = optionValue (args, "--file");
            auto ref = optionValue (args, "--ref");
            auto target = optionValue (args, "--target", "root");
            auto result = request (*sessionObject, "screenshot", object ({ { "file", file }, { "ref", ref }, { "target", target } }));
            printResult (result);
            return 0;
        }

        if (command == "click" && args.size() >= 1)
        {
            printResult (request (*sessionObject, "click", object ({ { "ref", args[0] } })));
            return 0;
        }

        if (command == "click-xy" && args.size() >= 2)
        {
            printResult (request (*sessionObject, "click_xy", object ({ { "x", args[0].getIntValue() }, { "y", args[1].getIntValue() } })));
            return 0;
        }

        if (command == "type" && args.size() >= 2)
        {
            auto ref = popFront (args);
            printResult (request (*sessionObject, "type", object ({ { "ref", ref }, { "text", args.joinIntoString (" ") } })));
            return 0;
        }

        if (command == "press" && args.size() >= 1)
        {
            auto ref = optionValue (args, "--ref");
            printResult (request (*sessionObject, "press", object ({ { "key", args[0] }, { "ref", ref } })));
            return 0;
        }

        if (command == "drag" && args.size() >= 1)
        {
            auto dx = optionValue (args, "--dx", "0").getIntValue();
            auto dy = optionValue (args, "--dy", "0").getIntValue();
            printResult (request (*sessionObject, "drag", object ({ { "ref", args[0] }, { "dx", dx }, { "dy", dy } })));
            return 0;
        }

        if (command == "set-bounds" && args.size() >= 1)
        {
            auto x = optionValue (args, "--x", "0").getIntValue();
            auto y = optionValue (args, "--y", "0").getIntValue();
            auto w = optionValue (args, "--w", "0").getIntValue();
            auto h = optionValue (args, "--h", "0").getIntValue();
            printResult (request (*sessionObject, "set_bounds", object ({ { "ref", args[0] }, { "x", x }, { "y", y }, { "w", w }, { "h", h } })));
            return 0;
        }

        if (command == "set-property" && args.size() >= 3)
        {
            printResult (request (*sessionObject, "set_property", object ({ { "ref", args[0] }, { "name", args[1] }, { "value", parseValue (args[2]) } })));
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
