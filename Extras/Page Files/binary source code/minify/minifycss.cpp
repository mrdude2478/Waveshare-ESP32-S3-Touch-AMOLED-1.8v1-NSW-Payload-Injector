#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <string>
#include <algorithm>

/*v22*/
/* Helper Function to trim leading/trailing whitespace from a string */
std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

/* ------------- The minifyCSS function (Single Pass: Final Polish) ------------- */
std::string minifyCSS(const std::string& css) {
    std::string result;
    bool inQuotes = false;
    char quoteChar = '\0';

    // State tracking: 
    bool inValueContext = false; // Tracks if we are inside a value (e.g., rgb(), url())
    size_t i = 0;
    while (i < css.length()) {
        char c = css[i];
        
        // --- Comment Handling /* ... */ ---
        if (!inQuotes && i + 1 < css.length() && c == '/' && css[i+1] == '*') {
            i += 2; // skip "/*"
            while (i + 1 < css.length() && !(css[i] == '*' && css[i+1] == '/')) {
                ++i;
            }
            if (i + 1 < css.length()) i += 2; // skip "*/"
        }
        // --- Quoted Strings ---
        else if (c == '"' || c == '\'') {
            if (!inQuotes) { // Opening quote
                inQuotes = true;
                quoteChar = c;
                inValueContext = true; 
            } else if (c == quoteChar) { // Closing quote
                inQuotes = false;
            }
            result += c;
        }

        // --- Whitespace Handling (THE MOST CRITICAL MODIFICATION) ---
        else if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes) {
            
            // 1. Check for structural tokens immediately following the space.
            // We look ahead to find the next non-whitespace character.
            size_t j = i + 1;
            while (j < css.length() && std::isspace(static_cast<unsigned char>(css[j]))) {
                ++j;
            }

            bool followedByStructuralToken = false;
            if (j < css.length()) {
                char nextChar = css[j];
                // Check if the character after all whitespace is a structural token 
                // that requires gluing to the preceding content.
                if (nextChar == '{' || nextChar == ':' || nextChar == ';' || 
                    nextChar == ',' || nextChar == '}' || nextChar == '(' || nextChar == ')') {
                    followedByStructuralToken = true;
                }
            }

            // If we are followed by a structural token, the space must be discarded.
            if (followedByStructuralToken) {
                ++i; 
                continue; // Skip the space entirely
            }


            // 2. Standard spacing rules (Only apply if not followed by a structural glue point)
            bool appendSpace = false; 
            
            if (inValueContext) {
                appendSpace = true; // e.g., rgb(10 px) -> needs space
            } else if ((i > 0 && css[i-1] == ',') || (c == ' ')) {
                 // If the preceding char was a comma, we might need one space separator.
                 if (i > 0 && css[i-1] == ',' && !result.empty() && result.back() != ' ') {
                     appendSpace = true;
                 }
            }

            if (appendSpace) {
                // Check if the last character already added was a space to prevent double spaces
                if (result.empty() || result.back() != ' ') {
                    result += ' ';
                }
            } else {
                 // Otherwise, skip the space entirely (this handles newlines/tabs too!)
                 ++i; 
                 continue;
            }
        }
        // --- Structural Appending & Space Removal ---
        else {
            // Append all characters normally. The structural tokens are handled here.
            result += c;
        }

        /* --- State Updates (Must happen AFTER processing 'c') --- */
        if (!inQuotes) {
            // Check for structural state changes based on the character we just processed:
            if (c == '{') {
                 inValueContext = true; 
            } else if (c == '}') {
                inValueContext = false;     
            } else if (c == ':' || c == ';') {
                 inValueContext = false; 
            } else if (!std::isspace(static_cast<unsigned char>(c)) && std::isalnum(static_cast<unsigned char>(c))) {
                 // If we see an identifier start, assume value context until proven otherwise.
                 inValueContext = true; 
            }
        }

        ++i;
    }
    return result;
}

/**
 * @brief Performs a final polish pass to remove spaces immediately following structural tokens like '{'.
 * This handles cases where the minifier might leave "{ " instead of "{}".
 */
std::string cleanStructuralSpaces(const std::string& css) {
    std::string cleaned = css;
    // The target pattern is: opening brace followed by a space.
    const std::string searchPattern = "{ "; 
    const std::string replacement = "{";

    size_t pos = cleaned.find(searchPattern);
    while (pos != std::string::npos) {
        // Replace the found pattern with the clean version
        cleaned.replace(pos, searchPattern.length(), replacement);
        
        // Continue searching from the point where the replacement occurred
        pos = cleaned.find(searchPattern, pos + replacement.length());
    }
    return cleaned;
}

/* ------------- command‑line file helpers -------------------------------- */
std::string readFile(const std::string& path) {
    // ... (Implementation remains the same) ...
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    // ... (Implementation remains the same) ...
    std::ofstream out(path, std::ios::out | std::ios::binary);
    if (!out) return false;
    out << data; 
    return true;
}

/* ------------- main ------------------------------------------------ */
int main(int argc, char** argv) {     
    // ... (main function remains the same) ...
    if (argc != 3) {
        std::cerr << "Usage: minify <input.css> <output.css>\n";
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

    // Step 0: Read the file
    std::string css_raw = readFile(inputPath);
    if (css_raw.empty()) {
        std::cerr << "Error: could not read '" << inputPath << "'\n";
        return 1;
    }

    // Step 1: Run the main minification pass
    std::string final_minified = minifyCSS(css_raw);
    
    // STEP 2: Post-processing Cleanup Pass (Fixes trailing spaces after '{')
    final_minified = cleanStructuralSpaces(final_minified);


    if (!writeFile(outputPath, final_minified)) {
        std::cerr << "Error: could not write '" << outputPath << "'\n";
        return 1;
    }

    std::cout << "Minification successful. Output written to: " << outputPath << '\n';
    return 0;
}
