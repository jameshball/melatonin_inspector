#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <limits>

#if !defined(_MSC_VER)
    #include <cxxabi.h>
namespace melatonin
{
    // https://stackoverflow.com/a/4541470
    static inline std::string demangle (const char* name)
    {
        int status = -4; // some arbitrary value to eliminate the compiler warning

        std::unique_ptr<char, void (*) (void*)> res {
            abi::__cxa_demangle (name, nullptr, nullptr, &status),
            std::free
        };

        return (status == 0) ? res.get() : name;
    }

    template <class T>
    static inline juce::String type (const T& t)
    {
        return demangle (typeid (t).name());
    }
}
#else
namespace melatonin
{
    template <class T>
    static inline juce::String type (const T& t)
    {
        return juce::String (typeid (t).name()).replace ("class ", "").replace ("struct ", "");
    }
}
#endif
namespace melatonin
{
    static inline juce::String trimComponentSuffix (juce::String name)
    {
        if (name.endsWith ("Component"))
            name = name.dropLastCharacters (9);

        if (name.endsWith ("Button"))
            name = name.dropLastCharacters (6) + " button";

        return name;
    }

    static inline juce::String humanizeIdentifier (juce::String name)
    {
        if (name.contains ("::"))
            name = name.fromLastOccurrenceOf ("::", false, false);

        if (name.contains ("<"))
            name = name.upToFirstOccurrenceOf ("<", false, false);

        name = trimComponentSuffix (name);

        juce::String result;
        juce::juce_wchar previous = 0;

        for (auto c : name)
        {
            if ((c == '_' || c == '-') && result.isNotEmpty())
            {
                result << " ";
                previous = ' ';
                continue;
            }

            if (previous != 0 && previous != ' ' && juce::CharacterFunctions::isUpperCase (c)
                && (juce::CharacterFunctions::isLowerCase (previous) || juce::CharacterFunctions::isDigit (previous)))
                result << " ";

            result << c;
            previous = c;
        }

        return result.trim();
    }

    static inline juce::String limitedComponentText (const juce::String& text)
    {
        auto trimmed = text.trim();
        return trimmed.length() > 80 ? trimmed.substring (0, 80) + "..." : trimmed;
    }

    static inline juce::String textForLabellingComponent (juce::Component& component)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&component))
            return limitedComponentText (label->getText());

        if (auto* group = dynamic_cast<juce::GroupComponent*> (&component))
            return limitedComponentText (group->getText());

        if (auto* button = dynamic_cast<juce::Button*> (&component))
            return limitedComponentText (button->getButtonText());

        if (auto* tooltip = dynamic_cast<juce::TooltipClient*> (&component))
            return limitedComponentText (tooltip->getTooltip());

        return {};
    }

    static inline int overlapOnAxis (int startA, int endA, int startB, int endB)
    {
        return juce::jmax (0, juce::jmin (endA, endB) - juce::jmax (startA, startB));
    }

    static inline juce::String nearbyLabelText (juce::Component& target)
    {
        auto* parent = target.getParentComponent();

        if (parent == nullptr)
            return {};

        const auto targetBounds = target.getBounds();
        juce::String best;
        auto bestScore = std::numeric_limits<int>::max();

        for (int i = 0; i < parent->getNumChildComponents(); ++i)
        {
            auto* sibling = parent->getChildComponent (i);

            if (sibling == nullptr || sibling == &target || ! sibling->isShowing())
                continue;

            const auto text = textForLabellingComponent (*sibling);

            if (text.isEmpty())
                continue;

            const auto bounds = sibling->getBounds();
            const auto verticalOverlap = overlapOnAxis (bounds.getY(), bounds.getBottom(), targetBounds.getY(), targetBounds.getBottom());
            const auto horizontalOverlap = overlapOnAxis (bounds.getX(), bounds.getRight(), targetBounds.getX(), targetBounds.getRight());
            auto score = std::numeric_limits<int>::max();

            if (verticalOverlap > 0 && bounds.getRight() <= targetBounds.getX())
                score = targetBounds.getX() - bounds.getRight();
            else if (horizontalOverlap > 0 && bounds.getBottom() <= targetBounds.getY())
                score = targetBounds.getY() - bounds.getBottom() + 1000;

            if (score >= 0 && score < bestScore && score < 1200)
            {
                best = text;
                bestScore = score;
            }
        }

        return best;
    }

    // do our best to derive a useful UI string from a component
    static inline juce::String componentString (juce::Component* c)
    {
        if (c == nullptr)
        {
            return "";
        }

#if JUCE_MODULE_AVAILABLE_juce_audio_processors
        if (auto editor = dynamic_cast<juce::AudioProcessorEditor*> (c))
        {
            return juce::String ("Editor: ") + editor->getAudioProcessor()->getName();
        }
#endif

        if (c->isAccessible() && c->getAccessibilityHandler() && !c->getAccessibilityHandler()->getTitle().isEmpty())
        {
            return c->getAccessibilityHandler()->getTitle();
        }

        if (!c->getName().isEmpty())
        {
            return c->getName();
        }

        if (auto* button = dynamic_cast<juce::Button*> (c))
        {
            auto text = limitedComponentText (button->getButtonText());

            if (text.isNotEmpty())
                return text;
        }

        if (auto* label = dynamic_cast<juce::Label*> (c))
        {
            auto text = limitedComponentText (label->getText());

            if (text.isNotEmpty())
                return text;
        }

        if (auto* group = dynamic_cast<juce::GroupComponent*> (c))
        {
            auto text = limitedComponentText (group->getText());

            if (text.isNotEmpty())
                return text;
        }

        if (auto* tooltip = dynamic_cast<juce::TooltipClient*> (c))
        {
            auto text = limitedComponentText (tooltip->getTooltip());

            if (text.isNotEmpty())
                return text;
        }

        if (auto text = nearbyLabelText (*c); text.isNotEmpty())
            return text;

        return humanizeIdentifier (type (*c));
    }

    // do our best to derive a useful UI string from a component
    static inline juce::String componentFontValue (juce::Component* c)
    {
        if (auto* label = dynamic_cast<juce::Label*> (c))
        {
            auto font = label->getFont();
            return juce::String (font.getTypefaceName() + " " + font.getTypefaceStyle() + " " + juce::String (font.getHeight()));
        }
        else if (auto btn = dynamic_cast<juce::TextButton*> (c))
        {
            auto font = btn->getLookAndFeel().getTextButtonFont (*btn, btn->getHeight());
            return juce::String (font.getTypefaceName() + " " + font.getTypefaceStyle() + " " + juce::String (font.getHeight()));
        }
        else if (auto slider = dynamic_cast<juce::Slider*> (c))
        {
            auto font = slider->getLookAndFeel().getSliderPopupFont (*slider);
            return juce::String (font.getTypefaceName() + " " + font.getTypefaceStyle() + " " + juce::String (font.getHeight()));
        }
        else if (auto cb = dynamic_cast<juce::ComboBox*> (c))
        {
            auto font = cb->getLookAndFeel().getComboBoxFont (*cb);
            return juce::String (font.getTypefaceName() + " " + font.getTypefaceStyle() + " " + juce::String (font.getHeight()));
        }
        else
        {
            return "-";
        }
    }

    // returns name of assigned LnF
    static inline juce::String lnfString (juce::Component* c)
    {
        if (c)
        {
            auto& lnf = c->getLookAndFeel();
            return type (lnf);
        }
        else
        {
            return "-";
        }
    }
}
