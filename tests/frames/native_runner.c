#include <stddef.h>
#include <unistd.h>

int test_startup_rollback(void);
int test_copy_latest_front(void);
int test_disable_validation_contention(void);
int test_max_frame(void);
int test_metrics_and_repeated_handover(void);
int test_fitted_viewport(void);

static void print_n(const char *text, size_t length)
{
    while (length) {
        const ssize_t written = write(STDOUT_FILENO, text, length);
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}

static void print(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    print_n(text, length);
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } cases[] = {
        {"startup rollback", test_startup_rollback},
        {"copied ownership / latest wins / front protection", test_copy_latest_front},
        {"disable / validation / nonblocking contention", test_disable_validation_contention},
        {"maximum frame", test_max_frame},
        {"proportional viewport / stable layout / letterbox excluded", test_fitted_viewport},
        {"metrics / 10000 repeated handovers", test_metrics_and_repeated_handover},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int line = cases[i].run();
        if (line) {
            char number[12];
            unsigned length = 0;
            do { number[length++] = (char)('0' + line % 10); line /= 10; } while (line);
            print("FAIL "); print(cases[i].name); print(" at frames_test.c:");
            while (length) print_n(&number[--length], 1);
            print("\n");
            return 1;
        }
        print("PASS "); print(cases[i].name); print("\n");
    }
    print("6 production app_frames.c regression groups passed.\n");
    return 0;
}
