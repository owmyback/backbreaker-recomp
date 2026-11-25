// Play numbers in each version of the game: see play_numbers.h.
#include "play_numbers.h"

#include <algorithm>
#include <cctype>

namespace play_numbers
{
    std::vector<std::string> filesOf(const std::string& xml)
    {
        static const std::string open = "<File>", close = "</File>";
        std::vector<std::string> out;
        size_t at = 0;
        while ((at = xml.find(open, at)) != std::string::npos)
        {
            at += open.size();
            const size_t end = xml.find(close, at);
            if (end == std::string::npos)
                break;
            std::string name = xml.substr(at, end - at);
            // as the game's XML reader gives it: without the white space around it
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            size_t lead = 0;
            while (lead < name.size() && std::isspace(static_cast<unsigned char>(name[lead]))) lead++;
            name.erase(0, lead);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            std::replace(name.begin(), name.end(), '/', '\\');
            out.push_back(std::move(name));
            at = end + close.size();
        }
        return out;
    }

    PlayList makeList(const std::string& offensiveXml, const std::string& defensiveXml, const std::string& specialXml)
    {
        PlayList list;
        list.files = filesOf(offensiveXml);
        list.offense = int(list.files.size());
        for (auto& f : filesOf(defensiveXml)) list.files.push_back(std::move(f));
        list.defense = int(list.files.size()) - list.offense;
        for (auto& f : filesOf(specialXml)) list.files.push_back(std::move(f));
        return list;
    }

    int translate(const PlayList& from, const PlayList& to, int number)
    {
        if (number < 0 || size_t(number) >= from.files.size())
            return -1;
        // a file listed twice: its first place
        const auto found = std::find(to.files.begin(), to.files.end(), from.files[size_t(number)]);
        return found == to.files.end() ? -1 : int(found - to.files.begin());
    }

    bool fromAnotherList(const PlayList& own, const Audibles& saved)
    {
        for (int i = 0; i < kSlots; i++)
            if ((saved.offense[i] != -1 && !own.isOffense(saved.offense[i])) ||
                (saved.defense[i] != -1 && !own.isDefense(saved.defense[i])))
                return true;
        return false;
    }

    Audibles renumber(const PlayList& other, const PlayList& own, const Audibles& saved)
    {
        Audibles out;
        for (int i = 0; i < kSlots; i++)
        {
            // a number on the wrong side of its own list isn't an audible the player could have set
            const int offense = other.isOffense(saved.offense[i]) ? translate(other, own, saved.offense[i]) : -1;
            const int defense = other.isDefense(saved.defense[i]) ? translate(other, own, saved.defense[i]) : -1;
            out.offense[i] = own.isOffense(offense) ? offense : -1;
            out.defense[i] = own.isDefense(defense) ? defense : -1;
        }
        return out;
    }
}
