// Startup line (06/11/2026, issue #10). Started by hand in a console, the engine printed nothing until "uci" and
// looked hung. It now prints one plain line with the same identity as the id name, "Triumviratus - 8.1 by Francesco
// Torsello", and a second line that points to the Credits of the repository, so contributors and testers are named
// too (author, 06/11/2026). Printed only when stdin is a terminal (main.cpp, g_startup_quiet): a GUI never sees it,
// because a line before "id name" can confuse strict GUIs. It replaces the ASCII-art box of version 5.1, commented
// out in main since then.
#include <cstdio>
#include "defs.h"
#include "presentation.h"

void print_banner() {
    printf("%s%s by %s\n", NAME, VERSION, AUTHOR);
    printf("Contributors and testers: see Credits at https://github.com/Tors3/Triumviratus\n");
    fflush(stdout);
}
