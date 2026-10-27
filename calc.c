// Aleksandrs Homko 251ADB142
// https://github.com/perkion1/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: ChatGPT (OpenAI, used before 27.10.2026), Claude (Anthropic, used from 27.10.2026, conversation: <https://claude.ai/share/44ecf87d-1cde-422b-86a6-4f3b75a53c97>), cppreference.com // ChatGPT conversation history cannot be provided: it was used on another computer and another person's account (my subscription had ended), which I currently do not have access to.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NAME "aleksandrs"
#define LASTNAME "homko"
#define STUDENT_ID "251ADB142"
#define PATH_LEN 4096

// A number is either an integer or a float. I keep a flag, because integer
// operations must stay integers (7 / 2 = 3) and only when one of the operands
// is a float the operation is done in double (7 / 2.0 = 3.5).
typedef struct {
    int is_float;
    long long i;
    double f;
} Number;

enum { NUM, PLUS, MINUS, MUL, DIV, POW, LPAREN, RPAREN, END, BAD };

// Parser state. The whole file is in one array (text) and pos is an index in
// that array. Because I never copy lines out of the array, the position of a
// token is just its index + 1, and newlines and comment lines are counted
// automatically.
static const char *text;
static size_t pos;       // index of the next character to read
static size_t line_end;  // index of '\n' at the end of this line (or file length)
static int tok;          // type of the current token
static size_t tok_pos;   // index where the current token starts
static Number tok_num;   // value if the current token is a number
static size_t error_pos; // 1-based position of the first error, 0 if no error

// Remembers only the first error, later errors on the same line are ignored.
static void set_error(size_t index) {
    if (error_pos == 0) {
        error_pos = index + 1;
    }
}

// Reads the next token. Tokens are read only when the parser needs them, so
// errors are found in the same order as the parser walks through the line.
static void next_token(void) {
    while (pos < line_end && (text[pos] == ' ' || text[pos] == '\t')) {
        pos++;
    }
    tok_pos = pos;
    if (pos >= line_end) {
        // End of line is also a token. Its position is the '\n' (or file length + 1),
        // so "missing operand" and "missing )" errors are reported there.
        tok = END;
        return;
    }

    char c = text[pos];
    if (isdigit((unsigned char)c)) {
        size_t begin = pos;
        while (pos < line_end && isdigit((unsigned char)text[pos])) {
            pos++;
        }
        tok = NUM;
        if (pos < line_end && text[pos] == '.') {
            // A float must have digits on both sides of the dot. In "5." the dot is
            // the wrong character, so the token is BAD and the error is at the dot.
            if (pos + 1 >= line_end || !isdigit((unsigned char)text[pos + 1])) {
                tok = BAD;
                tok_pos = pos;
                return;
            }
            // All digits are collected as one whole number and divided by 10 for
            // every digit after the dot: 12.5 = 125 / 10.
            double f = 0.0;
            double scale = 1.0;
            for (size_t k = begin; k < pos; k++) {
                f = f * 10 + (text[k] - '0');
            }
            pos++;
            while (pos < line_end && isdigit((unsigned char)text[pos])) {
                f = f * 10 + (text[pos] - '0');
                scale *= 10;
                pos++;
            }
            tok_num.is_float = 1;
            tok_num.f = f / scale;
            return;
        }
        long long value = 0;
        for (size_t k = begin; k < pos; k++) {
            value = value * 10 + (text[k] - '0');
        }
        tok_num.is_float = 0;
        tok_num.i = value;
        return;
    }

    pos++;
    if (c == '+') {
        tok = PLUS;
    } else if (c == '-') {
        tok = MINUS;
    } else if (c == '/') {
        tok = DIV;
    } else if (c == '(') {
        tok = LPAREN;
    } else if (c == ')') {
        tok = RPAREN;
    } else if (c == '*') {
        if (pos < line_end && text[pos] == '*') {
            pos++;
            tok = POW;
        } else {
            tok = MUL;
        }
    } else {
        // Everything else is invalid, also '#' when it is not at the start of a line.
        tok = BAD;
    }
}

static double to_double(Number n) {
    return n.is_float ? n.f : (double)n.i;
}

// Float results that are inf or nan are errors. They are reported at the
// operator, because both operands are fine numbers and only the operation fails.
static int check_float(double r, size_t op_pos, Number *out) {
    if (!isfinite(r)) {
        set_error(op_pos);
        return 0;
    }
    out->is_float = 1;
    out->f = r;
    return 1;
}

// Applies + - * / to a and b. Division by zero is reported at the start of the
// right operand, because the right operand is the wrong part.
static int calculate(int op, Number *a, Number b, size_t op_pos, size_t b_pos) {
    if (a->is_float || b.is_float) {
        double x = to_double(*a);
        double y = to_double(b);
        double r;
        if (op == PLUS) {
            r = x + y;
        } else if (op == MINUS) {
            r = x - y;
        } else if (op == MUL) {
            r = x * y;
        } else {
            if (y == 0.0) {
                set_error(b_pos);
                return 0;
            }
            r = x / y;
        }
        return check_float(r, op_pos, a);
    }

    if (op == PLUS) {
        a->i = a->i + b.i;
    } else if (op == MINUS) {
        a->i = a->i - b.i;
    } else if (op == MUL) {
        a->i = a->i * b.i;
    } else {
        if (b.i == 0) {
            set_error(b_pos);
            return 0;
        }
        a->i = a->i / b.i; // C already truncates toward zero
    }
    return 1;
}

// Integer power. I multiply by squaring the base, so a big exponent
// like 1 ** 1000000000 does not need a billion loop steps.
static long long int_power(long long base, long long exp) {
    long long result = 1;
    while (exp > 0) {
        if (exp % 2 == 1) {
            result *= base;
        }
        exp /= 2;
        if (exp > 0) {
            base *= base;
        }
    }
    return result;
}

// Grammar, one function for every precedence level:
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
// expr() can only add complete terms and term() can only multiply complete
// powers, so * and / are always done before + and -, and ** before both.
// The loops in expr() and term() go from left to right, so + - * / are
// left-associative.
// Every function also gives back where its expression starts (start), because
// some errors must be reported at the start of an operand.
static int expr(Number *out, size_t *start);

static int primary(Number *out, size_t *start) {
    *start = tok_pos;
    if (tok == NUM) {
        *out = tok_num;
        next_token();
        return 1;
    }
    if (tok == LPAREN) {
        next_token();
        size_t inner;
        if (!expr(out, &inner)) {
            return 0;
        }
        if (tok != RPAREN) {
            set_error(tok_pos); // at the end of the line this is the '\n'
            return 0;
        }
        next_token();
        return 1;
    }
    // An operand was expected but we got an operator, ')' , a bad character or
    // the end of the line. There is no unary minus, so "-5" also ends here.
    set_error(tok_pos);
    return 0;
}

// power() calls itself for the exponent, so 2 ** 3 ** 2 is read as
// 2 ** (3 ** 2): the right side is calculated first, that is why ** is
// right-associative.
static int power(Number *out, size_t *start) {
    if (!primary(out, start)) {
        return 0;
    }
    if (tok != POW) {
        return 1;
    }
    size_t op_pos = tok_pos;
    next_token();
    Number exp;
    size_t exp_pos;
    if (!power(&exp, &exp_pos)) {
        return 0;
    }
    if (!out->is_float && !exp.is_float) {
        if (exp.i < 0) {
            set_error(exp_pos); // negative integer exponent: at the start of the exponent
            return 0;
        }
        out->i = int_power(out->i, exp.i);
        return 1;
    }
    return check_float(pow(to_double(*out), to_double(exp)), op_pos, out);
}

static int term(Number *out, size_t *start) {
    if (!power(out, start)) {
        return 0;
    }
    while (tok == MUL || tok == DIV) {
        int op = tok;
        size_t op_pos = tok_pos;
        next_token();
        Number right;
        size_t right_pos;
        if (!power(&right, &right_pos)) {
            return 0;
        }
        if (!calculate(op, out, right, op_pos, right_pos)) {
            return 0;
        }
    }
    return 1;
}

static int expr(Number *out, size_t *start) {
    if (!term(out, start)) {
        return 0;
    }
    while (tok == PLUS || tok == MINUS) {
        int op = tok;
        size_t op_pos = tok_pos;
        next_token();
        Number right;
        size_t right_pos;
        if (!term(&right, &right_pos)) {
            return 0;
        }
        if (!calculate(op, out, right, op_pos, right_pos)) {
            return 0;
        }
    }
    return 1;
}

// Reads the whole file into memory with one malloc and one fread.
// The file is read only once and lines are never copied, so even a file of
// several megabytes is processed in one pass.
static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *data = malloc((size_t)len + 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    *size = fread(data, 1, (size_t)len, f);
    data[*size] = '\0';
    fclose(f);
    return data;
}

// Calculates every line of the input and writes the results to out_path.
// Results are written to the file right away. If an error happens, the file is
// opened again with "w", which makes it empty, and only ERROR:<pos> is written,
// because the output must not contain partial results.
static int process_file(const char *in_path, const char *out_path) {
    size_t size;
    char *data = read_file(in_path, &size);
    if (data == NULL) {
        fprintf(stderr, "calc: cannot read %s\n", in_path);
        return 0;
    }
    FILE *out = fopen(out_path, "w");
    if (out == NULL) {
        fprintf(stderr, "calc: cannot write %s\n", out_path);
        free(data);
        return 0;
    }

    text = data;
    error_pos = 0;
    size_t i = 0;
    while (i < size && error_pos == 0) {
        size_t end = i;
        while (end < size && data[end] != '\n') {
            end++;
        }

        size_t first = i;
        while (first < end && (data[first] == ' ' || data[first] == '\t')) {
            first++;
        }
        // Blank lines and comment lines give no output, but their characters
        // are still counted because i keeps going through the same array.
        if (first < end && data[first] != '#') {
            pos = i;
            line_end = end;
            next_token();
            Number result;
            size_t start;
            if (expr(&result, &start)) {
                if (tok != END) {
                    set_error(tok_pos); // something left after the expression, e.g. extra ')'
                } else if (result.is_float) {
                    fprintf(out, "%.15g\n", result.f);
                } else {
                    fprintf(out, "%lld\n", result.i);
                }
            }
        }
        i = end + 1;
    }

    if (error_pos != 0) {
        fclose(out);
        out = fopen(out_path, "w");
        if (out == NULL) {
            fprintf(stderr, "calc: cannot write %s\n", out_path);
            free(data);
            return 0;
        }
        fprintf(out, "ERROR:%zu\n", error_pos);
    }

    int ok = 1;
    if (fclose(out) != 0) {
        fprintf(stderr, "calc: cannot write %s\n", out_path);
        ok = 0;
    }
    free(data);
    return ok;
}

// Name of the file without folders and without ".txt", e.g. "labs/task1.txt" -> "task1".
static void get_base(const char *path, char *base) {
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    const char *slash = strrchr(tmp, '/');
    const char *name = slash ? slash + 1 : tmp;
    snprintf(base, PATH_LEN, "%s", name);
    len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".txt") == 0) {
        base[len - 4] = '\0';
    }
}

static int ends_with_txt(const char *name) {
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".txt") == 0;
}

// Creates the output folder if it does not exist yet.
static int make_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    if (mkdir(dir, 0777) != 0) {
        fprintf(stderr, "calc: cannot create folder %s: %s\n", dir, strerror(errno));
        return 0;
    }
    return 1;
}

static int run_one(const char *in_path, const char *out_dir) {
    char base[PATH_LEN];
    char out_path[PATH_LEN * 2];
    get_base(in_path, base);
    snprintf(out_path, sizeof out_path, "%s/%s_%s_%s_%s.txt",
             out_dir, base, NAME, LASTNAME, STUDENT_ID);
    return process_file(in_path, out_path);
}

static void usage(void) {
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char *argv[]) {
    const char *dir = NULL;
    const char *out_dir = NULL;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            out_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "calc: unknown option %s\n", argv[i]);
            usage();
            return 1;
        } else if (input == NULL) {
            input = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if ((dir == NULL && input == NULL) || (dir != NULL && input != NULL)) {
        usage();
        return 1;
    }

    // Check the input first, so nothing is created when the input is missing.
    struct stat st;
    const char *source = dir != NULL ? dir : input;
    if (stat(source, &st) != 0) {
        fprintf(stderr, "calc: %s does not exist\n", source);
        return 1;
    }
    if (dir != NULL && !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: %s is not a folder\n", dir);
        return 1;
    }

    // Default output folder: <input_base>_<name>_<studentid>
    char default_dir[PATH_LEN * 2];
    if (out_dir == NULL) {
        char base[PATH_LEN];
        get_base(source, base);
        snprintf(default_dir, sizeof default_dir, "%s_%s_%s", base, NAME, STUDENT_ID);
        out_dir = default_dir;
    }
    if (!make_dir(out_dir)) {
        return 1;
    }

    if (input != NULL) {
        return run_one(input, out_dir) ? 0 : 1;
    }

    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "calc: cannot open folder %s\n", dir);
        return 1;
    }
    int ok = 1;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!ends_with_txt(entry->d_name)) {
            continue;
        }
        char path[PATH_LEN * 2];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        // Only normal files directly in the folder, subfolders are skipped.
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!run_one(path, out_dir)) {
            ok = 0;
        }
    }
    closedir(d);
    return ok ? 0 : 1;
}
