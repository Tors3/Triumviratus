#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#define ISATTY(fd) _isatty(fd)
#define FILENO(f)  _fileno(f)
#else
#include <unistd.h>
#define ISATTY(fd) isatty(fd)
#define FILENO(f)  fileno(f)
#endif
#include "presentation.h"

int getConsoleWidth() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    return 80;
#endif
}

void printLine() {
    int w = getConsoleWidth();
    if (w < 1) w = 80;
    std::string line(w, '_');
    std::cout << "\033[38;2;205;133;63m" << line << "\033[0m" << std::endl;
}

// Banner di avvio. Pure-ASCII (rende ovunque), box generato dal codice
// (allineamento garantito), colori ANSI solo se stdout e' un terminale
// (in pipe verso un GUI stampa in chiaro, senza sporcare i log).
void ascii_art() {
    const int W = 60;  // larghezza interna del riquadro

    const bool tty = ISATTY(FILENO(stdout)) != 0;
    const std::string GOLD = tty ? "\033[38;2;218;165;32m" : "";
    const std::string DIM  = tty ? "\033[38;2;150;111;51m" : "";
    const std::string RST  = tty ? "\033[0m" : "";

    // Pezzo di scacchi a destra, testo a sinistra. Ogni riga <= W; il codice
    // riempie a destra fino a W, quindi il riquadro resta sempre allineato.
    std::vector<std::string> body = {
        "",
        "                                          (\\=,",
        "    T R I U M V I R A T U S              //  .\\",
        "    ==========================          (( \\_  \\",
        "                                         ))  `\\_)",
        "    Chess Engine                        (/     \\",
        "    Version 4.0                         | _.-'|",
        "                                         )___(",
        "    [+] NNUE Evaluation                 (=====)",
        "    [+] Syzygy Tablebases               }====={",
        "    [+] Lazy SMP Parallel Search       (_______)",
        "",
        "    by  Francesco  Torsello",
        "",
    };

    int indent = (getConsoleWidth() - (W + 4)) / 2;
    if (indent < 0) indent = 0;
    const std::string pad(indent, ' ');
    const std::string border = pad + "+" + std::string(W + 2, '=') + "+";

    std::cout << "\n" << GOLD << border << RST << "\n";
    for (std::string s : body) {
        if ((int)s.size() > W) s = s.substr(0, W);
        s.resize(W, ' ');
        std::cout << GOLD << pad << "| " << s << " |" << RST << "\n";
    }
    std::cout << GOLD << border << RST << "\n\n";
    std::cout.flush();
}
