#include <dsbox_cli/tokenize.hpp>

#include <cctype>
#include <cstddef>

namespace dsbox_cli
{
    std::vector<std::string> tokenizeCommandLine(std::string_view line)
    {
        std::vector<std::string> tokens;
        std::size_t i = 0;

        while (i < line.size())
        {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;

            if (i >= line.size())
                break;

            std::string token;

            if (line[i] == '"')
            {
                ++i;
                std::size_t end = line.find('"', i);

                if (end == std::string_view::npos)
                {
                    token.assign(line.substr(i));
                    i = line.size();
                }
                else
                {
                    token.assign(line.substr(i, end - i));
                    i = end + 1;
                }
            }
            else
            {
                std::size_t start = i;

                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;

                token.assign(line.substr(start, i - start));
            }

            tokens.push_back(std::move(token));
        }

        return tokens;
    }
}
