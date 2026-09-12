#include <fstream>
#include <iostream>
#include <string>
#include <cctype>
#include <set>
#include <algorithm>
#include <vector>
#include <random>
#include <sstream>

// Forward declarations
std::string minifyCSS(const std::string&, bool verbose);
std::string minifyJS(const std::string&, bool verbose);

// Safe char-to-lowercase helper. std::tolower/int-based tolower expects an
// unsigned char (or EOF) as input and returns an int; casting straight from
// char to char truncates the result and is technically UB for negative char
// values, which is what MSVC's C4244 warnings above are pointing at.
char toLowerChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Structure to hold search and replace pairs
struct Replacement {
    std::string search;
    std::string replace;
};

// Structure to hold preserved link tags
struct LinkTag {
    std::string content;
    std::string placeholder;
};

// Generate a unique placeholder
std::string generatePlaceholder(int index) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::stringstream ss;
    ss << "__LINKTAG_";
    for (int i = 0; i < 8; ++i) {
        ss << std::hex << dis(gen);
    }
    ss << "_" << index << "__";
    return ss.str();
}

// Extract <link> tags and replace with placeholders
void extractLinkTags(std::string& html, std::vector<LinkTag>& linkTags, bool verbose) {
    linkTags.clear();
    int linkCount = 0;
    size_t pos = 0;
    while (pos < html.length()) {
        if (pos + 4 < html.length() && html[pos] == '<') {
            std::string tag = html.substr(pos, 5);
            std::string tagLower = tag;
            std::transform(tagLower.begin(), tagLower.end(), tagLower.begin(), toLowerChar);
            if (tagLower == "<link") {
                std::string linkTag;
                size_t j = pos;
                bool inQuotes = false;
                char quoteChar = 0;
                while (j < html.length()) {
                    if (html[j] == '"' || html[j] == '\'') {
                        if (inQuotes && html[j] == quoteChar) {
                            inQuotes = false;
                        } else if (!inQuotes) {
                            inQuotes = true;
                            quoteChar = html[j];
                        }
                    } else if (!inQuotes && html[j] == '>' && (j == pos + 4 || html[j-1] != '/')) {
                        break; // End of opening tag
                    } else if (!inQuotes && j > pos + 4 && html[j] == '>' && html[j-1] == '/') {
                        break; // End of self-closing tag
                    }
                    linkTag += html[j];
                    j++;
                }
                linkTag += '>';
                std::string placeholder = generatePlaceholder(linkCount++);
                linkTags.push_back({linkTag, placeholder});
                html.replace(pos, linkTag.length(), placeholder);
                pos += placeholder.length();
                if (verbose) std::cerr << "Extracted <link> tag: " << linkTag << " with placeholder: " << placeholder << "\n";
                continue;
            }
        }
        pos++;
    }
}

// Load replacements from a file
std::vector<Replacement> loadReplacements(const std::string& replacementsFile, bool verbose) {
    std::vector<Replacement> replacements;
    std::ifstream file(replacementsFile);
    if (!file.is_open()) {
        if (verbose) std::cerr << "Warning: Cannot open replacements file " << replacementsFile << ", skipping replacements\n";
        return replacements;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        lineNumber++;
        if (line.empty() || line[0] == '#') continue;

        size_t delimPos = line.find('@');
        if (delimPos == std::string::npos) {
            if (verbose) std::cerr << "Warning: Skipping malformed line " << lineNumber << " in replacements file: '" << line << "' (missing ':')\n";
            continue;
        }

        std::string search = line.substr(0, delimPos);
        std::string replace = line.substr(delimPos + 1);
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(search);
        trim(replace);

        if (search.empty()) {
            if (verbose) std::cerr << "Warning: Skipping line " << lineNumber << " with empty search string\n";
            continue;
        }

        replacements.push_back({search, replace});
        if (verbose) std::cerr << "Loaded replacement: search='" << search << "', replace='" << replace << "'\n";
    }

    file.close();
    return replacements;
}

// Apply replacements, skipping placeholders
void applyReplacements(std::string& content, const std::vector<Replacement>& replacements, const std::vector<LinkTag>& linkTags, bool verbose) {
    for (const auto& r : replacements) {
        size_t pos = 0;
        size_t count = 0;
        while ((pos = content.find(r.search, pos)) != std::string::npos) {
            bool inPlaceholder = false;
            for (const auto& link : linkTags) {
                if (content.find(link.placeholder, pos) == pos) {
                    inPlaceholder = true;
                    break;
                }
            }
            if (inPlaceholder) {
                pos += r.search.length();
                continue;
            }
            content.replace(pos, r.search.length(), r.replace);
            pos += r.replace.length();
            count++;
            if (verbose) std::cerr << "Replaced '" << r.search << "' with '" << r.replace << "' at position " << pos << "\n";
        }
        if (verbose) std::cerr << "Total replacements for '" << r.search << "': " << count << "\n";
    }
}

std::string minifyCSS(const std::string& css, bool verbose) {
    (void)verbose; // Reserved for future verbose logging, unused for now; keeps the signature consistent with minifyJS.
    std::string result;
    bool inComment = false;
    bool inQuotes = false;
    char quoteChar = 0;
    size_t i = 0;

    while (i < css.length()) {
        char c = css[i];

        if (!inQuotes && i + 1 < css.length() && c == '/' && css[i+1] == '*') {
            inComment = true;
            i += 2;
            while (i + 1 < css.length() && !(css[i] == '*' && css[i+1] == '/')) {
                i++;
            }
            i += 2;
            inComment = false;
            continue;
        }

        if (inComment) {
            i++;
            continue;
        }

        if (c == '"' || c == '\'') {
            if (inQuotes && c == quoteChar) {
                inQuotes = false;
            } else if (!inQuotes) {
                inQuotes = true;
                quoteChar = c;
            }
            result += c;
            i++;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            if (!result.empty() && result.back() != ' ' && i + 1 < css.length() && 
                css[i+1] != '{' && css[i+1] != '}' && css[i+1] != ';' && css[i+1] != ':') {
                result += ' ';
            }
            i++;
            continue;
        }
        if (!inQuotes && c == ';' && i + 1 < css.length() && css[i+1] == '}') {
            i++;
            continue;
        }

        result += c;
        i++;
    }

    return result;
}

std::string minifyJS(const std::string& js, bool verbose) {
    std::string result;
    bool inSingleLineComment = false;
    bool inMultiLineComment = false;
    bool inQuotes = false;
    char quoteChar = 0;
    size_t i = 0;

    std::set<std::string> needsSpaceBefore = {
        "if", "else", "for", "while", "do", "switch", "case", "return", "function", 
        "var", "let", "const", "try", "catch", "finally", "throw", "new", "typeof"
    };
    std::set<char> operators = {
        '+', '-', '*', '/', '%', '=', '!', '&', '|', '^', '<', '>', '?', ':', ',', '.'
    };

    while (i < js.length()) {
        char c = js[i];

        if (!inQuotes && !inMultiLineComment && i + 1 < js.length() && c == '/' && js[i+1] == '/') {
            inSingleLineComment = true;
            i += 2;
            while (i < js.length() && js[i] != '\n') {
                i++;
            }
            inSingleLineComment = false;
            if (verbose) std::cerr << "Removed single-line comment\n";
            continue;
        }

        if (!inQuotes && !inSingleLineComment && i + 1 < js.length() && c == '/' && js[i+1] == '*') {
            inMultiLineComment = true;
            i += 2;
            while (i + 1 < js.length() && !(js[i] == '*' && js[i+1] == '/')) {
                i++;
            }
            i += 2;
            inMultiLineComment = false;
            if (verbose) std::cerr << "Removed multi-line comment\n";
            continue;
        }

        if (inSingleLineComment || inMultiLineComment) {
            i++;
            continue;
        }

        if (c == '"' || c == '\'') {
            if (inQuotes && c == quoteChar) {
                inQuotes = false;
            } else if (!inQuotes) {
                inQuotes = true;
                quoteChar = c;
            }
            result += c;
            i++;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            if (!result.empty() && i + 1 < js.length()) {
                char last = result.back();
                char next = js[i+1];
                bool needsSpace = false;

                if (std::isalnum(last) || last == '_') {
                    if (std::isalnum(next) || next == '_' || operators.find(next) != operators.end()) {
                        needsSpace = true;
                    } else {
                        size_t j = result.length();
                        std::string lastWord;
                        while (j > 0 && (std::isalnum(result[j-1]) || result[j-1] == '_')) {
                            lastWord = result[j-1] + lastWord;
                            j--;
                        }
                        if (needsSpaceBefore.find(lastWord) != needsSpaceBefore.end()) {
                            needsSpace = true;
                        }
                    }
                } else if (operators.find(last) != operators.end()) {
                    if (std::isalnum(next) || next == '_' || operators.find(next) != operators.end()) {
                        needsSpace = true;
                    }
                }

                if (needsSpace) {
                    result += ' ';
                    if (verbose) std::cerr << "Added necessary space at position " << i << "\n";
                }
            }
            i++;
            continue;
        }

        result += c;
        i++;
    }

    return result;
}

std::string minifyHTML(const std::string& html, bool aggressive, bool doMinifyJS, bool doMinifyCSS, bool preserveComments, bool verbose) {
    std::string result;
    bool inTag = false;
    bool inComment = false;
    bool inQuotes = false;
    bool inStyle = false;
    bool inScript = false;
    bool inPreserveWhitespaceTag = false;
    std::string currentTag;
    char quoteChar = 0;
    size_t i = 0;

    std::set<std::string> optionalTags = {
        "</p>", "</li>", "</dt>", "</dd>", "</tr>", "</td>", "</th>", "</thead>", 
        "</tbody>", "</tfoot>", "</colgroup>", "</optgroup>", "</option>", "</head>", "</body>"
    };

    std::set<std::string> booleanAttrs = {
        "disabled", "checked", "selected", "required", "readonly", "multiple", 
        "hidden", "open", "autoplay", "controls", "loop", "muted", "default"
    };

    std::set<std::string> jsAttrs = {
        "onclick", "onchange", "onsubmit", "onload", "onerror", "onfocus", "onblur"
    };

    std::set<std::string> preserveWhitespaceTags = {
        "title", "h1", "h2", "h3", "span", "label", "a", "button", "p"
    };

    std::set<std::string> preserveQuoteAttrs = {
        "rel", "href" // Preserve quotes for these attributes
    };

    while (i < html.length()) {
        char c = html[i];

        // Handle HTML comments
        if (!inQuotes && !inStyle && !inScript && i + 3 < html.length() && 
            c == '<' && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-') {
            inComment = true;
            std::string comment;
            i += 4;
            while (i + 2 < html.length() && !(html[i] == '-' && html[i+1] == '-' && html[i+2] == '>')) {
                comment += html[i];
                i++;
            }
            i += 3;
            if (preserveComments && comment.find("[if ") != std::string::npos) {
                result += "<!--" + comment + "-->";
                if (verbose) std::cerr << "Preserved conditional comment\n";
            } else if (verbose) {
                std::cerr << "Removed comment\n";
            }
            inComment = false;
            continue;
        }

        if (inComment) {
            i++;
            continue;
        }

        // Handle style tags
        if (!inQuotes && !inScript && i + 6 < html.length() && c == '<') {
            std::string tag = html.substr(i, 7);
            std::string tagLower = tag;
            std::transform(tagLower.begin(), tagLower.end(), tagLower.begin(), toLowerChar);
            if (tagLower == "<style>") {
                inStyle = true;
                std::string styleContent;
                i += 7;
                while (i < html.length()) {
                    if (i + 7 < html.length() && html[i] == '<') {
                        std::string endTag = html.substr(i, 8);
                        std::string endTagLower = endTag;
                        std::transform(endTagLower.begin(), endTagLower.end(), endTagLower.begin(), toLowerChar);
                        if (endTagLower == "</style>") break;
                    }
                    styleContent += html[i];
                    i++;
                }
                result += "<style>";
                result += doMinifyCSS ? minifyCSS(styleContent, verbose) : styleContent;
                result += "</style>";
                i += 8;
                if (verbose && doMinifyCSS) std::cerr << "Minified CSS in <style> tag\n";
                inStyle = false;
                continue;
            }
        }

        // Handle script tags
        if (!inQuotes && !inStyle && i + 7 < html.length() && c == '<') {
            std::string tag = html.substr(i, 8);
            std::string tagLower = tag;
            std::transform(tagLower.begin(), tagLower.end(), tagLower.begin(), toLowerChar);
            if (tagLower == "<script>") {
                inScript = true;
                std::string scriptContent;
                i += 8;
                while (i < html.length()) {
                    if (i + 8 < html.length() && html[i] == '<') {
                        std::string endTag = html.substr(i, 9);
                        std::string endTagLower = endTag;
                        std::transform(endTagLower.begin(), endTagLower.end(), endTagLower.begin(), toLowerChar);
                        if (endTagLower == "</script>") break;
                    }
                    scriptContent += html[i];
                    i++;
                }
                result += "<script>";
                result += doMinifyJS ? minifyJS(scriptContent, verbose) : scriptContent;
                result += "</script>";
                i += 9;
                if (verbose && doMinifyJS) std::cerr << "Minified JavaScript in <script> tag\n";
                inScript = false;
                continue;
            }
        }

        // Handle quoted strings (e.g., in attributes like onclick, style)
        if (inTag || inStyle || inScript) {
            if (c == '"' || c == '\'') {
                if (inQuotes && c == quoteChar) {
                    inQuotes = false;
                    size_t attrStart = result.length() - 1;
                    while (attrStart > 0 && result[attrStart] != ' ' && result[attrStart] != '<') {
                        attrStart--;
                    }
                    std::string attr = result.substr(attrStart + 1, result.length() - attrStart - 2);
                    if (doMinifyCSS && attr == "style") {
                        std::string styleValue;
                        size_t j = result.length() - 2;
                        while (j > 0 && result[j] != quoteChar) {
                            styleValue = result[j] + styleValue;
                            j--;
                        }
                        result = result.substr(0, j + 1);
                        result += minifyCSS(styleValue, verbose);
                        result += c;
                        if (verbose) std::cerr << "Minified inline style attribute\n";
                    } else if (doMinifyJS && jsAttrs.find(attr) != jsAttrs.end()) {
                        std::string jsValue;
                        size_t j = result.length() - 2;
                        while (j > 0 && result[j] != quoteChar) {
                            jsValue = result[j] + jsValue;
                            j--;
                        }
                        result = result.substr(0, j + 1);
                        result += minifyJS(jsValue, verbose);
                        result += c;
                        if (verbose) std::cerr << "Minified inline JS attribute (" << attr << ")\n";
                    } else {
                        result += c;
                    }
                } else if (!inQuotes) {
                    inQuotes = true;
                    quoteChar = c;
                    result += c;
                }
                i++;
                continue;
            }
        }

        // Track if inside a tag and identify preserve-whitespace tags
        if (!inQuotes && !inStyle && !inScript) {
            if (c == '<' && i + 1 < html.length()) {
                inTag = true;
                if (html[i+1] != '/') {
                    size_t j = i + 1;
                    std::string tagName;
                    while (j < html.length() && html[j] != ' ' && html[j] != '>' && html[j] != '/') {
                        tagName += toLowerChar(html[j]);
                        j++;
                    }
                    if (preserveWhitespaceTags.find(tagName) != preserveWhitespaceTags.end()) {
                        inPreserveWhitespaceTag = true;
                        currentTag = tagName;
                        if (verbose) std::cerr << "Entered whitespace-preserving tag: " << tagName << "\n";
                    }
                } else {
                    size_t j = i + 2;
                    std::string tagName;
                    while (j < html.length() && html[j] != ' ' && html[j] != '>') {
                        tagName += toLowerChar(html[j]);
                        j++;
                    }
                    if (inPreserveWhitespaceTag && tagName == currentTag) {
                        inPreserveWhitespaceTag = false;
                        currentTag.clear();
                        if (verbose) std::cerr << "Exited whitespace-preserving tag: " << tagName << "\n";
                    }
                }
            } else if (c == '>') {
                inTag = false;
            }
        }

        // Handle redundant attributes in aggressive mode
        if (aggressive && inTag && !inQuotes && !inStyle && !inScript && c == 't') {
            size_t j = i;
            std::string attr;
            while (j < html.length() && html[j] != '=' && html[j] != ' ' && html[j] != '>') {
                attr += html[j];
                j++;
            }
            if (attr == "type" && j + 1 < html.length() && html[j] == '=' && 
                (html[j+1] == '"' || html[j+1] == '\'')) {
                char q = html[j+1];
                std::string value;
                j += 2;
                while (j < html.length() && html[j] != q) {
                    value += html[j];
                    j++;
                }
                if ((inScript && value == "text/javascript") || (inStyle && value == "text/css")) {
                    i = j + 1;
                    if (verbose) std::cerr << "Removed redundant type attribute\n";
                    continue;
                }
            }
        }

        // Handle optional closing tags in aggressive mode
        if (aggressive && !inQuotes && !inStyle && !inScript && inTag && c == '<' && html[i+1] == '/') {
            std::string tag;
            size_t j = i;
            while (j < html.length() && html[j] != '>') {
                tag += toLowerChar(html[j]);
                j++;
            }
            tag += '>';
            if (optionalTags.find(tag) != optionalTags.end()) {
                i = j + 1;
                if (verbose) std::cerr << "Removed optional closing tag " << tag << "\n";
                continue;
            }
        }

        // Handle boolean attributes and quote removal in aggressive mode, preserving specific attributes and data: URLs
        if (aggressive && inTag && !inQuotes && !inStyle && !inScript && c == '=') {
            std::string attr;
            size_t j = i - 1;
            while (j > 0 && html[j] != ' ' && html[j] != '<') {
                attr = html[j] + attr;
                j--;
            }
            if (booleanAttrs.find(attr) != booleanAttrs.end()) {
                size_t k = i + 1;
                if (k < html.length() && (html[k] == '"' || html[k] == '\'')) {
                    std::string value;
                    char q = html[k];
                    k++;
                    while (k < html.length() && html[k] != q) {
                        value += html[k];
                        k++;
                    }
                    if (value == attr) {
                        i = k + 1;
                        result += attr;
                        if (verbose) std::cerr << "Shortened boolean attribute " << attr << "\n";
                        continue;
                    }
                }
            } else if (preserveQuoteAttrs.find(attr) != preserveQuoteAttrs.end() || 
                       ((html[i+1] == '"' || html[i+1] == '\'') && html.substr(i+2, 5) == "data:")) {
                std::string value;
                char q = html[i+1];
                size_t k = i + 2;
                while (k < html.length() && html[k] != q) {
                    value += html[k];
                    k++;
                }
                result += std::string(1, '=') + std::string(1, q) + value + std::string(1, q);
                i = k + 1;
                if (verbose) std::cerr << "Preserved quotes for attribute " << attr << "\n";
                continue;
            } else if (html[i+1] == '"' || html[i+1] == '\'') {
                std::string value;
                char q = html[i+1];
                size_t k = i + 2;
                while (k < html.length() && html[k] != q) {
                    value += html[k];
                    k++;
                }
                if (!value.empty()) {
                    bool safe = true;
                    for (char v : value) {
                        if (!std::isalnum(v) && v != '-' && v != '_' && v != '.') {
                            safe = false;
                            break;
                        }
                    }
                    if (safe) {
                        result += '=' + value;
                        i = k + 1;
                        if (verbose) std::cerr << "Removed quotes from attribute " << attr << "\n";
                        continue;
                    }
                }
            }
        }

        // Skip all whitespace outside tags, style, and script, unless in quotes or preserve-whitespace tag
        if (!inQuotes && !inTag && !inStyle && !inScript && !inPreserveWhitespaceTag && 
            (c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            i++;
            continue;
        }

        // In preserve-whitespace tags, keep single spaces between words
        if (!inQuotes && !inTag && !inStyle && !inScript && inPreserveWhitespaceTag && 
            (c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            if (!result.empty() && result.back() != ' ' && i + 1 < html.length() && 
                html[i+1] != '<' && html[i+1] != ' ' && html[i+1] != '\n' && html[i+1] != '\t' && html[i+1] != '\r') {
                result += ' ';
            }
            i++;
            continue;
        }

        result += c;
        i++;
    }

    return result;
}

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " -i <input_html_file> -o <output_html_file> [-r <replacements_file>] [--aggressive] [--minify-js] [--minify-css] [--all] [--preserve-comments] [--output-size] [--verbose] [--help]\n"
              << "  -i <file>            : Input HTML file\n"
              << "  -o <file>            : Output minified HTML file\n"
              << "  -r <file>            : File containing search:replace pairs (optional)\n"
              << "  --aggressive         : Remove optional tags, quotes, boolean attributes, and redundant type attributes\n"
              << "  --minify-js          : Minify JavaScript in <script> tags and inline attributes\n"
              << "  --minify-css         : Minify CSS in <style> tags and inline attributes\n"
              << "  --all                : Enable aggressive, minify-js, and minify-css\n"
              << "  --preserve-comments  : Preserve conditional comments (e.g., <!--[if IE]>)\n"
              << "  --output-size        : Report input and output file sizes\n"
              << "  --verbose            : Log minification and replacement details\n"
              << "  --help               : Show this help message\n";
}

int main(int argc, char* argv[]) {
    std::string inputFile, outputFile, replacementsFile;
    bool aggressive = false;
    bool doMinifyJS = false;
    bool doMinifyCSS = false;
    bool preserveComments = false;
    bool outputSize = false;
    bool verbose = false;

    // Manual argument parser (replaces getopt_long, which is unavailable on MSVC).
    // Supports short options that take a value (-i, -o, -r) either as a
    // separate token ("-i file") or attached ("-ifile"), and long flag
    // options (--aggressive, --minify-js, etc.) that take no value.
    auto needsValue = [](char c) { return c == 'i' || c == 'o' || c == 'r'; };

    for (int argi = 1; argi < argc; ++argi) {
        std::string arg = argv[argi];

        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            // Long option
            std::string name = arg.substr(2);
            if (name == "aggressive") {
                aggressive = true;
            } else if (name == "minify-js") {
                doMinifyJS = true;
            } else if (name == "minify-css") {
                doMinifyCSS = true;
            } else if (name == "all") {
                aggressive = true;
                doMinifyJS = true;
                doMinifyCSS = true;
            } else if (name == "preserve-comments") {
                preserveComments = true;
            } else if (name == "output-size") {
                outputSize = true;
            } else if (name == "verbose") {
                verbose = true;
            } else if (name == "help") {
                printUsage(argv[0]);
                return 0;
            } else {
                std::cerr << "Error: Unknown option --" << name << "\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (arg.size() >= 2 && arg[0] == '-') {
            // Short option, possibly with an attached value (-ifile.html)
            char opt = arg[1];
            std::string value = arg.substr(2);

            if (needsValue(opt)) {
                if (value.empty()) {
                    // Value is the next argument
                    if (argi + 1 >= argc) {
                        std::cerr << "Error: Option -" << opt << " requires a value\n";
                        printUsage(argv[0]);
                        return 1;
                    }
                    value = argv[++argi];
                }
                switch (opt) {
                    case 'i': inputFile = value; break;
                    case 'o': outputFile = value; break;
                    case 'r': replacementsFile = value; break;
                }
                continue;
            }

            switch (opt) {
                case 'a':
                    aggressive = true;
                    break;
                case 'j':
                    doMinifyJS = true;
                    break;
                case 'c':
                    doMinifyCSS = true;
                    break;
                case 'A':
                    aggressive = true;
                    doMinifyJS = true;
                    doMinifyCSS = true;
                    break;
                case 'p':
                    preserveComments = true;
                    break;
                case 's':
                    outputSize = true;
                    break;
                case 'v':
                    verbose = true;
                    break;
                case 'h':
                    printUsage(argv[0]);
                    return 0;
                default:
                    std::cerr << "Error: Unknown option -" << opt << "\n";
                    printUsage(argv[0]);
                    return 1;
            }
        } else {
            std::cerr << "Error: Unexpected argument '" << arg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (inputFile.empty() || outputFile.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    std::ifstream inFile(inputFile);
    if (!inFile.is_open()) {
        std::cerr << "Error: Cannot open input file " << inputFile << "\n";
        return 1;
    }

    std::string html((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    inFile.close();

    size_t inputSize = html.length();
    std::vector<LinkTag> linkTags;
    extractLinkTags(html, linkTags, verbose);
    std::string minified = minifyHTML(html, aggressive, doMinifyJS, doMinifyCSS, preserveComments, verbose);

    if (!replacementsFile.empty()) {
        std::vector<Replacement> replacements = loadReplacements(replacementsFile, verbose);
        if (!replacements.empty()) {
            applyReplacements(minified, replacements, linkTags, verbose);
            if (verbose) std::cerr << "Applied replacements from " << replacementsFile << "\n";
        }
    }

    // Restore link tags
    for (const auto& link : linkTags) {
        size_t pos = minified.find(link.placeholder);
        if (pos != std::string::npos) {
            minified.replace(pos, link.placeholder.length(), link.content);
            if (verbose) std::cerr << "Restored <link> tag: " << link.content << "\n";
        }
    }

    size_t outputSizeVal = minified.length();

    std::ofstream outFile(outputFile);
    if (!outFile.is_open()) {
        std::cerr << "Error: Cannot open output file " << outputFile << "\n";
        return 1;
    }

    outFile << minified;
    outFile.close();

    std::cout << "Minified HTML written to " << outputFile << "\n";
    if (outputSize) {
        std::cout << "Input size: " << inputSize << " bytes, Output size: " << outputSizeVal << " bytes, "
                  << "Compression ratio: " << (inputSize > 0 ? (1.0 - (double)outputSizeVal / inputSize) * 100 : 0) << "%\n";
    }

    return 0;
}