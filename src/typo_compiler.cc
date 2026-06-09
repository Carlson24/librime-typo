// src/typo_compiler.cc
#include <iostream>
#include <fstream>
#include <string>
#include <marisa.h>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: typo_compiler <input.txt> <output.bin>\n";
        return 1;
    }

    std::string input_path = argv[1];
    std::string output_path = argv[2];

    std::ifstream file(input_path);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open " << input_path << "\n";
        return 1;
    }

    marisa::Keyset keyset;
    std::string line;
    int count = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos != std::string::npos) {

            std::string typo = line.substr(0, tab_pos);
            std::string corrected = line.substr(tab_pos + 1);
            
            if (!corrected.empty() && corrected.back() == '\r') corrected.pop_back();
            if (!typo.empty() && typo.back() == '\r') typo.pop_back();

            if (!typo.empty() && !corrected.empty()) {
                std::string merged_key = typo + "\t" + corrected;
                keyset.push_back(merged_key.c_str(), merged_key.length());
                count++;
            }
        }
    }

    marisa::Trie trie;
    trie.build(keyset);
    trie.save(output_path.c_str());

    std::cout << "[Typo Compiler] Compiled " << count << " rules into " << output_path << "\n";
    return 0;
}