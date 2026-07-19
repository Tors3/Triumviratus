#include "defs.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
  #include "Windows.h"
  #include <io.h>
  #include <fcntl.h>
  #include <intrin.h>
  #include <conio.h>
#else
  #include <unistd.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <chrono>
#endif

// get time in milliseconds
int get_time_ms()
{
#ifdef _WIN32
    return GetTickCount();
#else
    using namespace std::chrono;
    return (int)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
#endif
}

int input_waiting()
{
#ifdef _WIN32
    return _kbhit();
#else
    // POSIX: stdin ha dati pronti? (poll non bloccante via select)
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) > 0;
#endif
}

// read GUI/user input
void read_input()
{
    if (input_waiting())
    {
        char input[256] = "";
        int  bytes;
#ifdef _WIN32
        bytes = _read(_fileno(stdin), input, 255);
#else
        bytes = read(fileno(stdin), input, 255);
#endif
        // EOF (pipe chiusa) o errore: NON fermare la ricerca. Su Linux select()
        // segnala "leggibile" anche all'EOF: trattarlo come stop abortiva la
        // ricerca a depth fissa subito -> nessuna mossa -> path di rescue.
        if (bytes <= 0) return;
        input[bytes] = '\0';
        char* endc = strchr(input, '\n');
        if (endc) *endc = '\0';
        // Solo i comandi UCI di interruzione fermano la ricerca.
        if (!strncmp(input, "quit", 4)) { quit = 1; stopped = 1; }
        else if (!strncmp(input, "stop", 4)) { stopped = 1; }
    }
}

// a bridge function to interact between search and GUI input
void communicate() {
    if (timeset == 1 && get_time_ms() > stoptime) {
        stopped = 1;
    }
    read_input();
}

// count bits within a bitboard. Build already requires BMI2/POPCNT (USE_PEXT is on),
// so the hardware instruction is always available -> no Kernighan fallback needed.
int count_bits(U64 bitboard)
{
#ifdef _WIN32
    return (int)__popcnt64(bitboard);
#else
    return __builtin_popcountll(bitboard);
#endif
}

// get least significant 1st bit index
int get_ls1b_index(U64 bitboard)
{
    if (bitboard)
    {
#ifdef _WIN32
        unsigned long index;
        _BitScanForward64(&index, bitboard);
        return static_cast<int>(index);
#else
        return __builtin_ctzll(bitboard);
#endif
    }
    else
        return -1;
}
