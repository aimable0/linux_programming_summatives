// program.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Requirement 5: one global variable ── */
int g_call_count = 0;

/* ── Three user-defined functions (excluding main) ── */

/* Function 1 – populates heap memory, contains a loop */
void fill_buffer(char *buf, int size) {
    g_call_count++;
    /* Requirement 2a: loop */
    for (int i = 0; i < size - 1; i++) {
        buf[i] = 'A' + (i % 26);   /* Requirement 3b: write to allocated memory */
    }
    buf[size - 1] = '\0';
}

/* Function 2 – searches buffer, contains a conditional branch */
int find_char(const char *buf, char target) {
    g_call_count++;
    int pos = -1;
    for (int i = 0; buf[i] != '\0'; i++) {
        /* Requirement 2b: conditional branch */
        if (buf[i] == target) {
            pos = i;
            break;
        }
    }
    return pos;
}

/* Function 3 – prints a summary, uses a standard library function (printf/strlen) */
void print_summary(const char *buf, int pos) {
    g_call_count++;
    /* Requirement 4: standard library functions – strlen, printf */
    size_t len = strlen(buf);
    printf("Buffer  : %s\n", buf);
    printf("Length  : %zu\n", len);
    printf("Position: %d\n", pos);
    printf("Calls   : %d\n", g_call_count);
}

/* ── main ── */
int main(void) {
    int size = 16;

    /* Requirement 3a: dynamic memory allocation */
    char *buf = (char *)malloc(size * sizeof(char));
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    fill_buffer(buf, size);
    int pos = find_char(buf, 'F');
    print_summary(buf, pos);

    free(buf);
    return 0;
}

