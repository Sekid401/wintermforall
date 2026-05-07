/*
 * winterm.c - Windows CMD emulator for Termux (Administrator / System32 edition)
 * compile: gcc winterm.c -o winterm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>

#define MAX_INPUT 4096
#define MAX_PATH  2048
#define MAX_VARS  256
#define MAX_ARGS  128

static char fs_root[MAX_PATH];
static char win_cwd[MAX_PATH];
static char real_cwd[MAX_PATH];

typedef struct { char key[64]; char val[256]; } Var;
static Var vars[MAX_VARS];
static int  var_count = 0;

/* ── Path conversion ─────────────────────────────────────────────────────── */

static void win_to_real(const char *wpath, char *out) {
    const char *p = wpath;
    if (strlen(p) >= 2 && p[1] == ':') p += 2;
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (int i = 0; tmp[i]; i++) if (tmp[i] == '\\') tmp[i] = '/';
    snprintf(out, MAX_PATH, "%s%s", fs_root, tmp);
}

static void real_to_win(const char *rpath, char *out) {
    const char *rel = rpath + strlen(fs_root);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    for (int i = 0; tmp[i]; i++) if (tmp[i] == '/') tmp[i] = '\\';
    snprintf(out, MAX_PATH, "C:%s", tmp[0] ? tmp : "\\");
}

/* ── Variable expansion ──────────────────────────────────────────────────── */

static void expand_vars(const char *in, char *out, size_t outsz) {
    char buf[MAX_INPUT * 2];
    int bi = 0;
    for (int i = 0; in[i] && bi < (int)outsz - 1; ) {
        if (in[i] == '%') {
            int j = i + 1;
            while (in[j] && in[j] != '%') j++;
            if (in[j] == '%') {
                char vname[64] = {0};
                int vlen = j - i - 1;
                if (vlen > 0 && vlen < 64) {
                    strncpy(vname, in + i + 1, vlen);
                    const char *val = NULL;
                    for (int k = 0; k < var_count; k++)
                        if (strcasecmp(vars[k].key, vname) == 0) { val = vars[k].val; break; }
                    if (!val) val = getenv(vname);
                    if (val) {
                        int vl = strlen(val);
                        if (bi + vl < (int)outsz - 1) { memcpy(buf + bi, val, vl); bi += vl; }
                    }
                }
                i = j + 1;
                continue;
            }
        }
        buf[bi++] = in[i++];
    }
    buf[bi] = 0;
    strncpy(out, buf, outsz);
}

/* ── Arg parser ──────────────────────────────────────────────────────────── */

static int parse_args(const char *input, char args[MAX_ARGS][MAX_PATH], int *argc) {
    *argc = 0;
    int i = 0, n = strlen(input);
    while (i < n && *argc < MAX_ARGS) {
        while (i < n && isspace((unsigned char)input[i])) i++;
        if (i >= n) break;
        int ai = 0;
        char *arg = args[*argc];
        if (input[i] == '"') {
            i++;
            while (i < n && input[i] != '"' && ai < MAX_PATH-1) arg[ai++] = input[i++];
            if (i < n) i++;
        } else {
            while (i < n && !isspace((unsigned char)input[i]) && ai < MAX_PATH-1) arg[ai++] = input[i++];
        }
        arg[ai] = 0;
        (*argc)++;
    }
    return *argc;
}

/* ── Helper: resolve path arg relative to cwd ────────────────────────────── */

static void resolve_path(const char *arg, char *real_out) {
    if (!arg || !*arg) {
        strncpy(real_out, real_cwd, MAX_PATH);
    } else if (strlen(arg) >= 2 && arg[1] == ':') {
        win_to_real(arg, real_out);
    } else if (arg[0] == '\\') {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof(tmp), "C:%s", arg);
        win_to_real(tmp, real_out);
    } else {
        snprintf(real_out, MAX_PATH, "%s/%s", real_cwd, arg);
    }
}

/* ── Commands ────────────────────────────────────────────────────────────── */

static void cmd_ver(void) {
    printf("Microsoft Windows [Version 10.0.19045.4291]\r\n"
           "(c) Microsoft Corporation. All rights reserved.\r\n");
}

static void cmd_cls(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void cmd_echo(const char *rest) {
    if (!rest || !*rest) { printf("ECHO is on.\r\n"); return; }
    printf("%s\r\n", rest);
}

static void cmd_dir(const char *path_arg) {
    char real[MAX_PATH];
    resolve_path(path_arg, real);

    DIR *d = opendir(real);
    if (!d) { printf("File Not Found\r\n"); return; }

    char wpath[MAX_PATH];
    real_to_win(real, wpath);
    printf("\r\n Directory of %s\r\n\r\n", wpath);

    long long total_files = 0, total_dirs = 0, total_size = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", real, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        struct tm *tm = localtime(&st.st_mtime);
        char date[32];
        strftime(date, sizeof(date), "%m/%d/%Y  %I:%M %p", tm);
        if (S_ISDIR(st.st_mode)) {
            printf("%s    <DIR>          %s\r\n", date, ent->d_name);
            total_dirs++;
        } else {
            printf("%s    %14lld %s\r\n", date, (long long)st.st_size, ent->d_name);
            total_files++;
            total_size += st.st_size;
        }
    }
    closedir(d);
    printf("%16lld File(s)  %lld bytes\r\n", total_files, total_size);
    printf("%16lld Dir(s)\r\n\r\n", total_dirs);
}

static void cmd_cd(const char *arg) {
    if (!arg || !*arg) { printf("%s\r\n", win_cwd); return; }

    char new_win[MAX_PATH];
    if (strcmp(arg, "..") == 0) {
        strncpy(new_win, win_cwd, sizeof(new_win));
        char *last = strrchr(new_win, '\\');
        if (last && last > new_win + 2) *last = 0;
        else if (last) { last[1] = 0; }
    } else if (strlen(arg) >= 2 && arg[1] == ':') {
        strncpy(new_win, arg, sizeof(new_win));
    } else if (arg[0] == '\\') {
        snprintf(new_win, sizeof(new_win), "C:%s", arg);
    } else {
        if (strcmp(win_cwd, "C:\\") == 0)
            snprintf(new_win, sizeof(new_win), "C:\\%s", arg);
        else
            snprintf(new_win, sizeof(new_win), "%s\\%s", win_cwd, arg);
    }
    for (int i = 0; new_win[i]; i++) if (new_win[i] == '/') new_win[i] = '\\';

    char new_real[MAX_PATH];
    win_to_real(new_win, new_real);
    struct stat st;
    if (stat(new_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("The system cannot find the path specified.\r\n");
        return;
    }
    strncpy(win_cwd, new_win, sizeof(win_cwd));
    strncpy(real_cwd, new_real, sizeof(real_cwd));
    chdir(real_cwd);
}

static void cmd_mkdir(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    if (mkdir(real, 0755) != 0) {
        if (errno == EEXIST) printf("A subdirectory or file %s already exists.\r\n", arg);
        else printf("The system cannot find the path specified.\r\n");
    }
}

static void cmd_rmdir(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    if (rmdir(real) != 0) printf("The directory is not empty or cannot be found.\r\n");
}

static void cmd_del(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    if (remove(real) != 0) printf("Could Not Find %s\r\n", arg);
}

static void cmd_type(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    FILE *f = fopen(real, "r");
    if (!f) { printf("The system cannot find the file specified.\r\n"); return; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) printf("%s", buf);
    fclose(f);
    printf("\r\n");
}

static void cmd_copy(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[0]);
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[1]);
    FILE *in = fopen(src, "rb"), *out_f = fopen(dst, "wb");
    if (!in)    { printf("The system cannot find the file specified.\r\n"); return; }
    if (!out_f) { fclose(in); printf("Access is denied.\r\n"); return; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out_f);
    fclose(in); fclose(out_f);
    printf("        1 file(s) copied.\r\n");
}

static void cmd_ren(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[0]);
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[1]);
    if (rename(src, dst) != 0) printf("The system cannot find the file specified.\r\n");
}

static void cmd_move(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[0]);
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[1]);
    if (rename(src, dst) != 0) printf("The file cannot be moved.\r\n");
    else printf("        1 file(s) moved.\r\n");
}

static void cmd_set(const char *arg) {
    if (!arg || !*arg) {
        for (int i = 0; i < var_count; i++) printf("%s=%s\r\n", vars[i].key, vars[i].val);
        return;
    }
    char *eq = strchr(arg, '=');
    if (!eq) {
        for (int i = 0; i < var_count; i++)
            if (strcasecmp(vars[i].key, arg) == 0) { printf("%s=%s\r\n", vars[i].key, vars[i].val); return; }
        printf("Environment variable %s not defined\r\n", arg);
        return;
    }
    char key[64] = {0};
    int klen = eq - arg < 63 ? (int)(eq - arg) : 63;
    strncpy(key, arg, klen);
    const char *val = eq + 1;
    for (int i = 0; i < var_count; i++) {
        if (strcasecmp(vars[i].key, key) == 0) {
            strncpy(vars[i].val, val, sizeof(vars[i].val)-1);
            return;
        }
    }
    if (var_count < MAX_VARS) {
        strncpy(vars[var_count].key, key, 63);
        strncpy(vars[var_count].val, val, 255);
        var_count++;
    }
}

static void cmd_date_cmd(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%A %m/%d/%Y", tm);
    printf("The current date is: %s\r\n", buf);
}

static void cmd_time_cmd(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M:%S.00", tm);
    printf("The current time is: %s\r\n", buf);
}

static void cmd_help(void) {
    printf(
        "For more information on a specific command, type HELP command-name\r\n\r\n"
        "ARP         Displays and modifies the ARP cache.\r\n"
        "ASSOC       Displays or modifies file extension associations.\r\n"
        "AT          Schedules commands (deprecated, use SCHTASKS).\r\n"
        "ATTRIB      Displays or changes file attributes.\r\n"
        "BCDEDIT     Sets properties in boot database.\r\n"
        "BOOTREC     Repairs the master boot record.\r\n"
        "BREAK       Sets or clears extended CTRL+C checking.\r\n"
        "CALL        Calls one batch program from another.\r\n"
        "CD          Displays or changes the current directory.\r\n"
        "CHKDSK      Checks a disk and displays a status report.\r\n"
        "CHKNTFS     Displays or modifies the checking of disk at boot time.\r\n"
        "CHOICE      Accepts user input to a batch program.\r\n"
        "CIPHER      Displays or alters encryption of files on NTFS.\r\n"
        "CLIP        Redirects output of command line tools to the clipboard.\r\n"
        "CLS         Clears the screen.\r\n"
        "COLOR       Sets the default console foreground and background colors.\r\n"
        "COMP        Compares the contents of two files or sets of files.\r\n"
        "COMPACT     Displays or alters the compression of files on NTFS.\r\n"
        "CONVERT     Converts FAT volumes to NTFS.\r\n"
        "COPY        Copies one or more files to another location.\r\n"
        "DATE        Displays or sets the date.\r\n"
        "DEFRAG      Defragments and optimizes local volumes.\r\n"
        "DEL         Deletes one or more files.\r\n"
        "DIR         Displays a list of files and subdirectories.\r\n"
        "DISKPART    Displays or configures Disk Partition properties.\r\n"
        "DISM        Deployment Image Servicing and Management tool.\r\n"
        "DOSKEY      Edits command lines, recalls commands, and creates macros.\r\n"
        "DRIVERQUERY Displays current device driver status and properties.\r\n"
        "ECHO        Displays messages, or turns command-echoing on or off.\r\n"
        "ENDLOCAL    Ends localization of environment changes in a batch file.\r\n"
        "ERASE       Deletes one or more files.\r\n"
        "EXPAND      Expands one or more compressed files.\r\n"
        "EXIT        Quits CMD.EXE.\r\n"
        "FC          Compares two files or sets of files and displays differences.\r\n"
        "FIND        Searches for a text string in a file or files.\r\n"
        "FINDSTR     Searches for strings in files.\r\n"
        "FOR         Runs a command for each file in a set of files.\r\n"
        "FORMAT      Formats a disk for use with Windows.\r\n"
        "FSUTIL      Displays or configures file system properties.\r\n"
        "FTYPE       Displays or modifies file types used in file extension assoc.\r\n"
        "GOTO        Directs CMD to a labeled line in a batch program.\r\n"
        "GPRESULT    Displays Group Policy information for machine or user.\r\n"
        "HELP        Provides Help information for Windows commands.\r\n"
        "HOSTNAME    Prints the name of the current host.\r\n"
        "ICACLS      Display, modify, backup, or restore ACLs for files/dirs.\r\n"
        "IF          Performs conditional processing in batch programs.\r\n"
        "IPCONFIG    Display all current TCP/IP network configuration values.\r\n"
        "LABEL       Creates, changes, or deletes the volume label of a disk.\r\n"
        "MD          Creates a directory.\r\n"
        "MKDIR       Creates a directory.\r\n"
        "MKLINK      Creates Symbolic Links and Hard Links.\r\n"
        "MORE        Displays output one screen at a time.\r\n"
        "MOVE        Moves one or more files from one directory to another.\r\n"
        "MSIEXEC     Installer tool for Windows Installer packages.\r\n"
        "MSCONFIG    System Configuration utility.\r\n"
        "NET         Manages network resources, users, and services.\r\n"
        "NETSH       Configures network settings.\r\n"
        "NETSTAT     Displays active TCP connections and ports.\r\n"
        "NSLOOKUP    DNS lookup utility.\r\n"
        "PATH        Displays or sets a search path for executable files.\r\n"
        "PATHPING    Provides network latency and packet loss info.\r\n"
        "PAUSE       Suspends processing of a batch program.\r\n"
        "PING        Tests network connectivity.\r\n"
        "PKTMON      Packet Monitor - Windows 10 packet capture tool.\r\n"
        "POPD        Restores the previous value of the current directory.\r\n"
        "PRINT       Prints a text file.\r\n"
        "PROMPT      Changes the Windows command prompt.\r\n"
        "PUSHD       Saves the current directory then changes it.\r\n"
        "RD          Removes a directory.\r\n"
        "REG         Registry operations (ADD/DELETE/QUERY/COPY/SAVE/LOAD).\r\n"
        "REGEDIT     Registry editor.\r\n"
        "REGSVR32    Registers or unregisters OLE controls.\r\n"
        "REM         Records comments in batch files or CONFIG.SYS.\r\n"
        "REN         Renames a file or files.\r\n"
        "RENAME      Renames a file or files.\r\n"
        "REPLACE     Replaces files.\r\n"
        "RMDIR       Removes a directory.\r\n"
        "ROBOCOPY    Advanced utility to copy files and directory trees.\r\n"
        "ROUTE       Manipulates network routing tables.\r\n"
        "RUNAS       Allows a user to run specific tools and programs with different permissions.\r\n"
        "SC          Displays or configures services.\r\n"
        "SCHTASKS    Schedules commands and programs to run on a computer.\r\n"
        "SET         Displays, sets, or removes Windows environment variables.\r\n"
        "SETLOCAL    Begins localization of environment changes in a batch file.\r\n"
        "SFC         System File Checker - verifies integrity of Windows files.\r\n"
        "SHIFT       Shifts the position of replaceable parameters in batch files.\r\n"
        "SHUTDOWN    Allows proper local or remote shutdown of machine.\r\n"
        "SORT        Sorts input.\r\n"
        "START       Starts a separate window to run a specified program.\r\n"
        "SUBST       Associates a path with a drive letter.\r\n"
        "SYSTEMINFO  Displays machine specific properties and configuration.\r\n"
        "TAKEOWN     Allows an administrator to recover access to a denied file.\r\n"
        "TASKKILL    Kill or stop a running process or application.\r\n"
        "TASKLIST    Displays all currently running tasks including services.\r\n"
        "TIME        Displays or sets the system time.\r\n"
        "TIMEOUT     Waits for the specified time, or until a key is pressed.\r\n"
        "TITLE       Sets the window title for a CMD.EXE session.\r\n"
        "TRACERT     Determines the path taken to a destination.\r\n"
        "TREE        Graphically displays the directory structure of a drive.\r\n"
        "TYPE        Displays the contents of a text file.\r\n"
        "VER         Displays the Windows version.\r\n"
        "VERIFY      Tells Windows whether to verify files are written correctly.\r\n"
        "VOL         Displays a disk volume label and serial number.\r\n"
        "WHERE       Displays the location of files that match a search pattern.\r\n"
        "WHOAMI      Displays user, group and privileges information.\r\n"
        "WINVER      Displays the Windows version dialog.\r\n"
        "WMIC        Displays WMI information inside interactive command shell.\r\n"
        "XCOPY       Copies files and directory trees.\r\n"
    );
}

static void cmd_tree_rec(const char *rp, const char *pfx, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(rp);
    if (!d) return;
    char names[512][256]; int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && cnt < 512) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", rp, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            strncpy(names[cnt++], ent->d_name, 255);
    }
    closedir(d);
    for (int i = 0; i < cnt; i++) {
        int last = (i == cnt - 1);
        printf("%s%s%s\r\n", pfx, last ? "\\---" : "+---", names[i]);
        char full[MAX_PATH], newpfx[512];
        snprintf(full, sizeof(full), "%s/%s", rp, names[i]);
        snprintf(newpfx, sizeof(newpfx), "%s%s", pfx, last ? "    " : "|   ");
        cmd_tree_rec(full, newpfx, depth + 1);
    }
}

static void cmd_tree(const char *arg) {
    char real[MAX_PATH];
    if (!arg || !*arg) strncpy(real, real_cwd, sizeof(real));
    else snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    char wpath[MAX_PATH];
    real_to_win(real, wpath);
    printf("Folder PATH listing\r\n%s\r\n", wpath);
    cmd_tree_rec(real, "", 0);
}

/* ── New commands ────────────────────────────────────────────────────────── */

static void cmd_attrib(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    /* Parse leading +/- flags then filename */
    char flags[32] = {0};
    const char *p = arg;
    int fi = 0;
    while (*p && (*p == '+' || *p == '-' || isalpha((unsigned char)*p)) && *p != ' ' && fi < 31)
        flags[fi++] = *p++;
    while (*p == ' ') p++;
    char real[MAX_PATH];
    if (*p) snprintf(real, sizeof(real), "%s/%s", real_cwd, p);
    else    snprintf(real, sizeof(real), "%s/*", real_cwd);

    struct stat st;
    if (*p && stat(real, &st) != 0) { printf("File not found - %s\r\n", p); return; }
    if (fi == 0) {
        /* display mode */
        printf("A                    %s\r\n", *p ? p : "(current directory)");
    } else {
        printf("Attribute(s) set for: %s\r\n", *p ? p : "(current directory)");
    }
}

static void cmd_assoc(const char *arg) {
    /* Minimal registry-style association table */
    static const char *assoc_table[][2] = {
        {".exe", "exefile"}, {".bat", "batfile"}, {".cmd", "cmdfile"},
        {".txt", "txtfile"}, {".log", "txtfile"}, {".ini", "inifile"},
        {".com", "comfile"}, {".dll", "dllfile"}, {".sys", "sysfile"},
        {".zip", "CompressedFolder"}, {".cab", "CABFolder"},
        {".xml", "xmlfile"}, {".htm", "htmlfile"}, {".html", "htmlfile"},
        {".pdf", "AcroExch.Document"}, {".mp3", "mp3file"}, {".wav", "wavfile"},
        {".jpg", "jpegfile"}, {".jpeg", "jpegfile"}, {".png", "pngfile"},
        {".bmp", "bmpfile"}, {".gif", "giffile"}, {".reg", "regfile"},
        {".ps1", "Microsoft.PowerShellScript.1"}, {NULL, NULL}
    };
    if (!arg || !*arg) {
        for (int i = 0; assoc_table[i][0]; i++)
            printf("%s=%s\r\n", assoc_table[i][0], assoc_table[i][1]);
        return;
    }
    char *eq = strchr(arg, '=');
    if (!eq) {
        for (int i = 0; assoc_table[i][0]; i++)
            if (strcasecmp(assoc_table[i][0], arg) == 0) {
                printf("%s=%s\r\n", assoc_table[i][0], assoc_table[i][1]);
                return;
            }
        printf("File association not found for extension %s\r\n", arg);
    } else {
        printf("File association set (simulated).\r\n");
    }
}

static void dispatch(const char *input); /* forward-declare */

static void cmd_call(const char *arg) {
    if (!arg || !*arg) return;
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s.bat", real_cwd, arg);
    FILE *f = fopen(real, "r");
    if (!f) {
        /* try exact name */
        snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
        f = fopen(real, "r");
    }
    if (!f) { printf("The system cannot find the file specified.\r\n"); return; }
    char line[MAX_INPUT];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        dispatch(line);
    }
    fclose(f);
}

static void cmd_chkdsk(const char *arg) {
    (void)arg;
    printf("The type of the file system is NTFS.\r\n\r\n"
           "WARNING!  F parameter not specified.\r\n"
           "Running CHKDSK in read-only mode.\r\n\r\n"
           "Stage 1: Examining basic file system structure ...\r\n"
           "  262144 file records processed.\r\n\r\n"
           "Stage 2: Examining file name linkage ...\r\n"
           "  295478 index entries processed.\r\n\r\n"
           "Stage 3: Examining security descriptors ...\r\n"
           "Security descriptor verification completed.\r\n\r\n"
           "Windows has scanned the file system and found no problems.\r\n"
           " 122880000 KB total disk space.\r\n"
           "  58732032 KB in use.\r\n"
           "  64147968 KB available on disk.\r\n"
           "      4096 bytes in each allocation unit.\r\n"
           "  30720000 total allocation units on disk.\r\n"
           "  16036992 allocation units available on disk.\r\n");
}

static void cmd_chkntfs(const char *arg) {
    (void)arg;
    printf("The type of the file system is NTFS.\r\n"
           "C: is not dirty.\r\n");
}

static void cmd_color(const char *arg) {
    if (!arg || !*arg) {
        /* reset */
        printf("\033[0m");
        return;
    }
    /* arg is two hex digits: background foreground */
    int bg = 7, fg = 0;
    if (strlen(arg) >= 2) {
        char bgc = toupper((unsigned char)arg[0]);
        char fgc = toupper((unsigned char)arg[1]);
        /* Map Windows color indices to ANSI (best-effort) */
        static const int win_to_ansi[16] = {0,4,2,6,1,5,3,7,8,12,10,14,9,13,11,15};
        int bgi = (bgc >= '0' && bgc <= '9') ? bgc - '0' : bgc - 'A' + 10;
        int fgi = (fgc >= '0' && fgc <= '9') ? fgc - '0' : fgc - 'A' + 10;
        if (bgi < 0 || bgi > 15) bgi = 0;
        if (fgi < 0 || fgi > 15) fgi = 7;
        bg = win_to_ansi[bgi];
        fg = win_to_ansi[fgi];
        (void)bg; (void)fg;
        printf("\033[%dm\033[%dm", (fg < 8 ? 30 + fg : 90 + fg - 8), (bg < 8 ? 40 + bg : 100 + bg - 8));
    }
}

static void cmd_comp(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char p1[MAX_PATH], p2[MAX_PATH];
    snprintf(p1, sizeof(p1), "%s/%s", real_cwd, args[0]);
    snprintf(p2, sizeof(p2), "%s/%s", real_cwd, args[1]);
    FILE *f1 = fopen(p1, "rb"), *f2 = fopen(p2, "rb");
    if (!f1 || !f2) {
        if (f1) fclose(f1); if (f2) fclose(f2);
        printf("The system cannot find the file specified.\r\n"); return;
    }
    int diffs = 0; long off = 0;
    int c1, c2;
    while ((c1 = fgetc(f1)) != EOF && (c2 = fgetc(f2)) != EOF) {
        if (c1 != c2) {
            printf("Compare error at OFFSET %08lX\r\n  file1 = %02X\r\n  file2 = %02X\r\n", off, c1, c2);
            diffs++;
            if (diffs >= 10) { printf("10 Mismatches - ending compare\r\n"); break; }
        }
        off++;
    }
    fclose(f1); fclose(f2);
    if (!diffs) printf("Files compare OK\r\n");
}

static void cmd_compact(const char *arg) {
    (void)arg;
    printf("Listing %s\\\r\n\r\n"
           "New files added to this directory will not be compressed.\r\n\r\n"
           "  0 files within 1 directories are compressed.\r\n", win_cwd);
}

static void cmd_convert(const char *arg) {
    (void)arg;
    printf("The type of the file system is NTFS.\r\n"
           "Volume is already NTFS. CONVERT will not perform the conversion.\r\n");
}

static void cmd_diskpart(void) {
    printf("Microsoft DiskPart version 10.0.19041.964\r\n\r\n"
           "Copyright (C) Microsoft Corporation.\r\n"
           "On computer: DESKTOP-WINTERM\r\n\r\n");
    fflush(stdout);

    int sel_disk = -1, sel_vol = -1, sel_part = -1;
    char line[512];

    for (;;) {
        printf("DISKPART> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (!*cmd) continue;

        char up[512];
        strncpy(up, cmd, sizeof(up)-1); up[sizeof(up)-1] = 0;
        for (int i = 0; up[i]; i++) up[i] = toupper((unsigned char)up[i]);

        if (strcmp(up, "LIST DISK") == 0) {
            printf("\r\n"
                   "  Disk ###  Status         Size     Free     Dyn  Gpt\r\n"
                   "  --------  -------------  -------  -------  ---  ---\r\n"
                   "  Disk 0    Online          120 GB      0 B         *\r\n\r\n");

        } else if (strcmp(up, "LIST VOLUME") == 0) {
            printf("\r\n"
                   "  Volume ###  Ltr  Label        Fs     Type        Size     Status     Info\r\n"
                   "  ----------  ---  -----------  -----  ----------  -------  ---------  --------\r\n"
                   "  Volume 0     C   OS           NTFS   Partition   119 GB   Healthy    Boot\r\n"
                   "  Volume 1         SYSTEM       FAT32  Partition   100 MB   Healthy    System\r\n"
                   "  Volume 2         Recovery     NTFS   Partition   529 MB   Healthy    Hidden\r\n\r\n");

        } else if (strcmp(up, "LIST PARTITION") == 0) {
            if (sel_disk < 0)
                printf("\r\nThere is no disk selected to list partitions on.\r\nSelect a disk and try again.\r\n\r\n");
            else
                printf("\r\n"
                       "  Partition ###  Type              Size     Offset\r\n"
                       "  -------------  ----------------  -------  -------\r\n"
                       "  Partition 1    System             100 MB  1024 KB\r\n"
                       "  Partition 2    Reserved            16 MB   101 MB\r\n"
                       "  Partition 3    Primary            119 GB   117 MB\r\n"
                       "  Partition 4    Recovery           529 MB   119 GB\r\n\r\n");

        } else if (strcmp(up, "LIST VDISK") == 0) {
            printf("\r\n  There are no virtual disks to show.\r\n\r\n");

        } else if (strncmp(up, "SELECT DISK", 11) == 0) {
            int n = 0;
            if (sscanf(up + 11, " %d", &n) == 1 && n == 0) {
                sel_disk = 0; sel_vol = -1; sel_part = -1;
                printf("\r\nDisk %d is now the selected disk.\r\n\r\n", n);
            } else printf("\r\nThe specified disk is not valid.\r\n\r\n");

        } else if (strncmp(up, "SELECT VOLUME", 13) == 0) {
            int n = 0;
            if (sscanf(up + 13, " %d", &n) == 1 && n >= 0 && n <= 2) {
                sel_vol = n; sel_part = -1;
                printf("\r\nVolume %d is the selected volume.\r\n\r\n", n);
            } else printf("\r\nThe specified volume is not valid.\r\n\r\n");

        } else if (strncmp(up, "SELECT PARTITION", 16) == 0) {
            int n = 0;
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\nSelect a disk and try again.\r\n\r\n");
            else if (sscanf(up + 16, " %d", &n) == 1 && n >= 1 && n <= 4) {
                sel_part = n;
                printf("\r\nPartition %d is now the selected partition.\r\n\r\n", n);
            } else printf("\r\nThe specified partition is not valid.\r\n\r\n");

        } else if (strcmp(up, "DETAIL DISK") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nVMware Virtual disk SCSI Disk Device\r\n"
                        "Disk ID: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}\r\n"
                        "Type   : SCSI\r\nStatus : Online\r\nPath   : 0\r\n"
                        "Target : 0\r\nLUN ID : 0\r\n"
                        "Current Read-only State : No\r\nRead-only  : No\r\n"
                        "Boot Disk  : Yes\r\nPagefile Disk  : Yes\r\n"
                        "Hibernation File Disk  : No\r\nCrashdump Disk  : Yes\r\n"
                        "Clustered Disk  : No\r\n\r\n"
                        "  Volume ###  Ltr  Label        Fs     Type        Size     Status     Info\r\n"
                        "  ----------  ---  -----------  -----  ----------  -------  ---------  --------\r\n"
                        "  Volume 0     C   OS           NTFS   Partition   119 GB   Healthy    Boot\r\n\r\n");

        } else if (strcmp(up, "DETAIL VOLUME") == 0) {
            if (sel_vol < 0) printf("\r\nThere is no volume selected.\r\n\r\n");
            else printf("\r\nRead-only              : No\r\nHidden                 : No\r\n"
                        "No Default Drive Letter: No\r\nShadow Copy            : No\r\n"
                        "Offline                : No\r\nBitLocker Encrypted    : No\r\n"
                        "Installable            : Yes\r\n\r\n"
                        "  Disk ###  Status         Size     Free     Dyn  Gpt\r\n"
                        "  --------  -------------  -------  -------  ---  ---\r\n"
                        "* Disk 0    Online          120 GB      0 B         *\r\n\r\n");

        } else if (strcmp(up, "DETAIL PARTITION") == 0) {
            if (sel_part < 0) printf("\r\nThere is no partition selected.\r\n\r\n");
            else printf("\r\nPartition %d\r\nType    : 07\r\nHidden  : No\r\n"
                        "Required: No\r\nAttrib  : 0X8000000000000000\r\n"
                        "Offset in Bytes: 117440512\r\n\r\n"
                        "  Volume ###  Ltr  Label        Fs     Type        Size     Status     Info\r\n"
                        "  ----------  ---  -----------  -----  ----------  -------  ---------  --------\r\n"
                        "* Volume 0     C   OS           NTFS   Partition   119 GB   Healthy    Boot\r\n\r\n",
                        sel_part);

        } else if (strcmp(up, "ACTIVE") == 0) {
            if (sel_part < 0) printf("\r\nThere is no partition selected.\r\n\r\n");
            else printf("\r\nDiskPart marked the current partition as active.\r\n\r\n");

        } else if (strcmp(up, "INACTIVE") == 0) {
            if (sel_part < 0) printf("\r\nThere is no partition selected.\r\n\r\n");
            else printf("\r\nDiskPart marked the current partition as inactive.\r\n\r\n");

        } else if (strncmp(up, "ATTRIBUTES DISK", 15) == 0) {
            printf("\r\nCurrent Read-only State : No\r\nRead-only  : No\r\n"
                   "Boot Disk  : Yes\r\nPagefile Disk  : Yes\r\n"
                   "Hibernation File Disk  : No\r\nCrashdump Disk  : Yes\r\n"
                   "Clustered Disk  : No\r\n\r\n");

        } else if (strncmp(up, "ATTRIBUTES VOLUME", 17) == 0) {
            printf("\r\nRead-only              : No\r\nHidden                 : No\r\n"
                   "No Default Drive Letter: No\r\nShadow Copy            : No\r\n\r\n");

        } else if (strcmp(up, "CLEAN") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart succeeded in cleaning the disk.\r\n\r\n");

        } else if (strcmp(up, "CLEAN ALL") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart is formatting the volume...\r\n"
                        "DiskPart succeeded in cleaning the disk.\r\n\r\n");

        } else if (strncmp(up, "CREATE PARTITION", 16) == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart succeeded in creating the specified partition.\r\n\r\n");

        } else if (strncmp(up, "CREATE VOLUME", 13) == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully created the volume.\r\n\r\n");

        } else if (strncmp(up, "DELETE PARTITION", 16) == 0) {
            if (sel_part < 0) printf("\r\nThere is no partition selected.\r\n\r\n");
            else { printf("\r\nDiskPart successfully deleted the selected partition.\r\n\r\n"); sel_part = -1; }

        } else if (strcmp(up, "DELETE VOLUME") == 0) {
            if (sel_vol < 0) printf("\r\nThere is no volume selected.\r\n\r\n");
            else { printf("\r\nDiskPart successfully deleted the volume.\r\n\r\n"); sel_vol = -1; }

        } else if (strncmp(up, "FORMAT", 6) == 0) {
            if (sel_vol < 0 && sel_part < 0)
                printf("\r\nThere is no volume or partition selected.\r\n\r\n");
            else
                printf("\r\n  100 percent completed\r\n\r\n"
                       "DiskPart successfully formatted the volume.\r\n\r\n");

        } else if (strncmp(up, "ASSIGN", 6) == 0) {
            if (sel_vol < 0 && sel_part < 0)
                printf("\r\nThere is no volume or partition selected.\r\n\r\n");
            else
                printf("\r\nDiskPart successfully assigned the drive letter or mount point.\r\n\r\n");

        } else if (strncmp(up, "REMOVE", 6) == 0) {
            if (sel_vol < 0 && sel_part < 0)
                printf("\r\nThere is no volume or partition selected.\r\n\r\n");
            else
                printf("\r\nDiskPart successfully removed the drive letter or mount point.\r\n\r\n");

        } else if (strncmp(up, "EXTEND", 6) == 0) {
            if (sel_vol < 0 && sel_part < 0)
                printf("\r\nThere is no volume or partition selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully extended the volume.\r\n\r\n");

        } else if (strncmp(up, "SHRINK", 6) == 0) {
            if (sel_vol < 0 && sel_part < 0)
                printf("\r\nThere is no volume or partition selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully shrunk the volume.\r\n\r\n");

        } else if (strcmp(up, "CONVERT GPT") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully converted the selected disk to GPT format.\r\n\r\n");

        } else if (strcmp(up, "CONVERT MBR") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully converted the selected disk to MBR format.\r\n\r\n");

        } else if (strcmp(up, "CONVERT DYNAMIC") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully converted the selected disk to dynamic format.\r\n\r\n");

        } else if (strcmp(up, "CONVERT BASIC") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully converted the selected disk to basic format.\r\n\r\n");

        } else if (strcmp(up, "ONLINE DISK") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully onlined the selected disk.\r\n\r\n");

        } else if (strcmp(up, "OFFLINE DISK") == 0) {
            if (sel_disk < 0) printf("\r\nThere is no disk selected.\r\n\r\n");
            else printf("\r\nDiskPart successfully offlined the selected disk.\r\n\r\n");

        } else if (strcmp(up, "RESCAN") == 0) {
            printf("\r\nPlease wait while DiskPart scans your configuration...\r\n"
                   "DiskPart has finished scanning your configuration.\r\n\r\n");

        } else if (strncmp(up, "UNIQUEID DISK", 13) == 0) {
            printf("\r\nDisk ID: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}\r\n\r\n");

        } else if (strcmp(up, "HELP") == 0 || strcmp(up, "?") == 0) {
            printf("\r\n"
                   "ACTIVE      - Mark the selected partition as active.\r\n"
                   "ASSIGN      - Assign a drive letter or mount point.\r\n"
                   "ATTRIBUTES  - Manipulate volume or disk attributes.\r\n"
                   "CLEAN       - Clear all configuration info off the disk.\r\n"
                   "CONVERT     - Convert between GPT, MBR, dynamic, basic.\r\n"
                   "CREATE      - Create a volume, partition or virtual disk.\r\n"
                   "DELETE      - Delete an object.\r\n"
                   "DETAIL      - Provide details about an object.\r\n"
                   "EXIT        - Exit DiskPart.\r\n"
                   "EXTEND      - Extend a volume.\r\n"
                   "FORMAT      - Format the volume or partition.\r\n"
                   "INACTIVE    - Mark the selected partition as inactive.\r\n"
                   "LIST        - Display a list of objects.\r\n"
                   "OFFLINE     - Take an object offline.\r\n"
                   "ONLINE      - Take an object online.\r\n"
                   "REMOVE      - Remove a drive letter or mount point.\r\n"
                   "RESCAN      - Rescan the computer for disks and volumes.\r\n"
                   "SELECT      - Shift the focus to an object.\r\n"
                   "SHRINK      - Reduce the size of the selected volume.\r\n"
                   "UNIQUEID    - Display or set the GPT identifier.\r\n\r\n");

        } else if (strcmp(up, "EXIT") == 0 || strcmp(up, "QUIT") == 0) {
            printf("\r\nLeaving DiskPart...\r\n\r\n");
            break;

        } else {
            printf("\r\nThe syntax of the command is incorrect.\r\n"
                   "For more information on this command, type: HELP %s\r\n\r\n", up);
        }
    }
}

static void cmd_dism(const char *arg) {
    (void)arg;
    printf("Deployment Image Servicing and Management tool\r\n"
           "Version: 10.0.19041.844\r\n\r\n"
           "Image Version: 10.0.19041.1\r\n\r\n"
           "The operation completed successfully.\r\n");
}

static void cmd_doskey(const char *arg) {
    (void)arg;
    printf("DOSKEY macros and history management not available in this emulator.\r\n");
}

static void cmd_driverquery(void) {
    printf("Module Name  Display Name           Driver Type   Link Date\r\n"
           "============ ====================== ============= =====================\r\n"
           "1394ohci     1394 OHCI Compliant ... Kernel        12/7/2019 4:09:02 AM\r\n"
           "3ware        3ware                  Kernel        2/26/2007 6:43:43 PM\r\n"
           "ACPI         Microsoft ACPI Driver  Kernel        12/7/2019 4:18:11 AM\r\n"
           "AcpiDev      ACPI Devices driver    Kernel        12/7/2019 3:53:45 AM\r\n"
           "acpiex       Microsoft ACPIEx Driv  Kernel        12/7/2019 3:53:45 AM\r\n"
           "acpipagr     ACPI Processor Aggreg  Kernel        12/7/2019 3:53:45 AM\r\n"
           "AcpiPmi      ACPI Power Meter Driv  Kernel        12/7/2019 3:50:11 AM\r\n"
           "...\r\n");
}

static void cmd_fc(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char p1[MAX_PATH], p2[MAX_PATH];
    snprintf(p1, sizeof(p1), "%s/%s", real_cwd, args[0]);
    snprintf(p2, sizeof(p2), "%s/%s", real_cwd, args[1]);
    FILE *f1 = fopen(p1, "r"), *f2 = fopen(p2, "r");
    if (!f1 || !f2) {
        if (f1) fclose(f1); if (f2) fclose(f2);
        printf("FC: cannot open %s - No such file or directory\r\n", !f1 ? args[0] : args[1]); return;
    }
    printf("Comparing files %s and %s\r\n", args[0], args[1]);
    char l1[MAX_INPUT], l2[MAX_INPUT];
    int diffs = 0, lineno = 1;
    while (fgets(l1, sizeof(l1), f1) && fgets(l2, sizeof(l2), f2)) {
        l1[strcspn(l1,"\r\n")] = 0; l2[strcspn(l2,"\r\n")] = 0;
        if (strcmp(l1, l2) != 0) {
            printf("***** %s line %d:\r\n%s\r\n***** %s line %d:\r\n%s\r\n*****\r\n",
                   args[0], lineno, l1, args[1], lineno, l2);
            diffs++;
        }
        lineno++;
    }
    fclose(f1); fclose(f2);
    if (!diffs) printf("FC: no differences encountered\r\n");
}

static void cmd_find(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    const char *needle = args[0];
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, args[1]);
    FILE *f = fopen(real, "r");
    if (!f) { printf("FIND: File not found - %s\r\n", args[1]); return; }
    printf("---------- %s\r\n", args[1]);
    char line[MAX_INPUT];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strstr(line, needle)) printf("%s\r\n", line);
    }
    fclose(f);
}

static void cmd_findstr(const char *args_str) {
    /* simplified: findstr pattern file */
    cmd_find(args_str);
}

static void cmd_for_cmd(const char *rest) {
    /* Very basic: FOR %V IN (a b c) DO echo %V */
    (void)rest;
    printf("FOR command: basic iteration not fully supported in this emulator.\r\n");
}

static void cmd_format(const char *arg) {
    (void)arg;
    printf("The type of the file system is NTFS.\r\n"
           "WARNING: ALL DATA ON NON-REMOVABLE DISK\r\n"
           "DRIVE C: WILL BE LOST!\r\n"
           "Proceed with Format (Y/N)? ");
    fflush(stdout);
    char c[8];
    if (fgets(c, sizeof(c), stdin) && (c[0] == 'y' || c[0] == 'Y'))
        printf("Format cancelled (protected by emulator).\r\n");
    else
        printf("Format cancelled.\r\n");
}

static void cmd_ftype(const char *arg) {
    static const char *ftype_table[][2] = {
        {"exefile",   "C:\\Windows\\System32\\cmd.exe /c \"%1\" %*"},
        {"batfile",   "C:\\Windows\\System32\\cmd.exe /c \"%1\" %*"},
        {"cmdfile",   "C:\\Windows\\System32\\cmd.exe /c \"%1\" %*"},
        {"txtfile",   "C:\\Windows\\System32\\notepad.exe %1"},
        {"htmlfile",  "C:\\Program Files\\Internet Explorer\\iexplore.exe %1"},
        {NULL, NULL}
    };
    if (!arg || !*arg) {
        for (int i = 0; ftype_table[i][0]; i++)
            printf("%s=%s\r\n", ftype_table[i][0], ftype_table[i][1]);
        return;
    }
    for (int i = 0; ftype_table[i][0]; i++)
        if (strcasecmp(ftype_table[i][0], arg) == 0) {
            printf("%s=%s\r\n", ftype_table[i][0], ftype_table[i][1]);
            return;
        }
    printf("File type '%s' not found or no open command associated with it.\r\n", arg);
}

static void cmd_gpresult(const char *arg) {
    (void)arg;
    printf("\r\nMicrosoft (R) Windows (R) Operating System Group Policy Result tool v2.0\r\n"
           "(C) Microsoft Corporation. All rights reserved.\r\n\r\n"
           "Created on %s\r\n\r\n"
           "RSOP data for DESKTOP-WINTERM\\Administrator\r\n"
           "-----------------------------------------------\r\n"
           "    OS Configuration:            Primary Domain Controller\r\n"
           "    OS Version:                  10.0.19045\r\n"
           "    Site Name:                   N/A\r\n"
           "    Roaming Profile:             N/A\r\n"
           "    Local Profile:               C:\\Users\\Administrator\r\n"
           "    Connected over a slow link?: No\r\n", "05/07/2026 12:00:00");
}

static void cmd_hostname(void) {
    printf("DESKTOP-WINTERM\r\n");
}

static void cmd_icacls(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    struct stat st;
    if (stat(real, &st) != 0) { printf("No files with matching names were found.\r\n"); return; }
    printf("%s BUILTIN\\Administrators:(I)(F)\r\n"
           "      NT AUTHORITY\\SYSTEM:(I)(F)\r\n"
           "      NT AUTHORITY\\Authenticated Users:(I)(M)\r\n"
           "      BUILTIN\\Users:(I)(RX)\r\n\r\n"
           "Successfully processed 1 files; Failed processing 0 files\r\n", arg);
}

static void cmd_ipconfig(const char *arg) {
    int all = arg && (strcasecmp(arg, "/all") == 0);
    printf("Windows IP Configuration\r\n\r\n");
    if (all) {
        printf("   Host Name . . . . . . . . . . . . : DESKTOP-WINTERM\r\n"
               "   Primary Dns Suffix  . . . . . . . :\r\n"
               "   Node Type . . . . . . . . . . . . : Hybrid\r\n"
               "   IP Routing Enabled. . . . . . . . : No\r\n"
               "   WINS Proxy Enabled. . . . . . . . : No\r\n\r\n");
    }
    printf("Ethernet adapter Ethernet:\r\n\r\n"
           "   Connection-specific DNS Suffix  . :\r\n"
           "   IPv4 Address. . . . . . . . . . . : 192.168.1.100\r\n"
           "   Subnet Mask . . . . . . . . . . . : 255.255.255.0\r\n"
           "   Default Gateway . . . . . . . . . : 192.168.1.1\r\n\r\n"
           "Ethernet adapter Loopback Pseudo-Interface 1:\r\n\r\n"
           "   Connection-specific DNS Suffix  . :\r\n"
           "   IPv6 Address. . . . . . . . . . . : ::1\r\n"
           "   IPv4 Address. . . . . . . . . . . : 127.0.0.1\r\n"
           "   Subnet Mask . . . . . . . . . . . : 255.0.0.0\r\n"
           "   Default Gateway . . . . . . . . . :\r\n");
}

static void cmd_label(const char *arg) {
    if (!arg || !*arg) {
        printf("Volume in drive C is WINDOWS\r\n"
               "Volume Serial Number is 1A2B-3C4D\r\n"
               "Volume label (11 characters, ENTER for none)? ");
        fflush(stdout);
        char buf[16]; fgets(buf, sizeof(buf), stdin);
    } else {
        printf("Volume label set to %s (simulated).\r\n", arg);
    }
}

static void cmd_mklink(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char dst[MAX_PATH], src[MAX_PATH];
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[0]);
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[1]);
    if (symlink(src, dst) != 0) {
        if (errno == EEXIST) printf("Cannot create a file when that file already exists.\r\n");
        else printf("Access is denied.\r\n");
    } else {
        printf("symbolic link created for %s <<===>> %s\r\n", args[0], args[1]);
    }
}

static void cmd_net(const char *rest) {
    if (!rest || !*rest) {
        printf("The syntax of this command is:\r\n\r\n"
               "NET [ ACCOUNTS | COMPUTER | CONFIG | CONTINUE | FILE | GROUP | HELP |\r\n"
               "      HELPMSG | LOCALGROUP | PAUSE | SESSION | SHARE | START |\r\n"
               "      STATISTICS | STOP | TIME | USE | USER | VIEW ]\r\n"); return;
    }
    char sub[32] = {0};
    const char *p = rest;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;

    if (strcmp(sub, "USER") == 0) {
        if (!*p) {
            printf("\r\nUser accounts for \\\\DESKTOP-WINTERM\r\n"
                   "-------------------------------------------------------------------------------\r\n"
                   "Administrator            DefaultAccount           Guest\r\n"
                   "WDAGUtilityAccount\r\n"
                   "The command completed successfully.\r\n");
        } else {
            printf("User name                    %s\r\n"
                   "Full Name\r\n"
                   "Comment\r\n"
                   "User's comment\r\n"
                   "Country/region code          000 (System Default)\r\n"
                   "Account active               Yes\r\n"
                   "Account expires              Never\r\n\r\n"
                   "The command completed successfully.\r\n", p);
        }
    } else if (strcmp(sub, "START") == 0) {
        printf("Starting %s service...\r\nThe %s service was started successfully.\r\n", p, p);
    } else if (strcmp(sub, "STOP") == 0) {
        printf("Stopping %s service...\r\nThe %s service was stopped successfully.\r\n", p, p);
    } else if (strcmp(sub, "LOCALGROUP") == 0) {
        printf("\r\nAliases for \\\\DESKTOP-WINTERM\r\n"
               "-------------------------------------------------------------------------------\r\n"
               "*Administrators\r\n*Backup Operators\r\n*Cryptographic Operators\r\n"
               "*Device Owners\r\n*Distributed COM Users\r\n*Event Log Readers\r\n"
               "*Guests\r\n*Hyper-V Administrators\r\n*IIS_IUSRS\r\n"
               "*Network Configuration Operators\r\n*Performance Log Users\r\n"
               "*Performance Monitor Users\r\n*Power Users\r\n*Remote Desktop Users\r\n"
               "*Remote Management Users\r\n*Replicator\r\n*System Managed Accounts Group\r\n"
               "*Users\r\n"
               "The command completed successfully.\r\n");
    } else if (strcmp(sub, "SHARE") == 0) {
        printf("Share name   Resource                        Remark\r\n\r\n"
               "-------------------------------------------------------------------------------\r\n"
               "C$           C:\\                             Default share\r\n"
               "IPC$                                         Remote IPC\r\n"
               "ADMIN$       C:\\Windows                      Remote Admin\r\n"
               "The command completed successfully.\r\n");
    } else if (strcmp(sub, "VIEW") == 0) {
        printf("Server Name            Remark\r\n\r\n"
               "-------------------------------------------------------------------------------\r\n"
               "\\\\DESKTOP-WINTERM\r\n"
               "The command completed successfully.\r\n");
    } else {
        printf("System error 5 has occurred.\r\nAccess is denied.\r\n");
    }
}

static void cmd_netstat(const char *arg) {
    int show_all = arg && (strstr(arg, "-a") || strstr(arg, "/a"));
    printf("\r\nActive Connections\r\n\r\n"
           "  Proto  Local Address          Foreign Address        State\r\n"
           "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING\r\n"
           "  TCP    0.0.0.0:445            0.0.0.0:0              LISTENING\r\n"
           "  TCP    0.0.0.0:5040           0.0.0.0:0              LISTENING\r\n"
           "  TCP    127.0.0.1:1001         0.0.0.0:0              LISTENING\r\n"
           "  TCP    192.168.1.100:139      0.0.0.0:0              LISTENING\r\n");
    if (show_all) {
        printf("  TCP    192.168.1.100:49816   52.239.209.165:443     ESTABLISHED\r\n"
               "  TCP    192.168.1.100:50322   13.107.42.14:443       ESTABLISHED\r\n"
               "  UDP    0.0.0.0:5353          *:*\r\n"
               "  UDP    0.0.0.0:5355          *:*\r\n");
    }
    printf("\r\n");
}

static void cmd_path(const char *arg) {
    if (!arg || !*arg) {
        printf("PATH=C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem;"
               "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\;"
               "C:\\Windows\\System32\\OpenSSH\\\r\n");
    } else {
        cmd_set(arg); /* treat as SET PATH=... */
        printf("PATH set successfully.\r\n");
    }
}

static void cmd_pathping(const char *arg) {
    if (!arg || !*arg) { printf("Usage: PATHPING target_name\r\n"); return; }
    printf("Tracing route to %s over a maximum of 30 hops\r\n\r\n"
           "  0  DESKTOP-WINTERM [192.168.1.100]\r\n"
           "  1  router.home [192.168.1.1]\r\n"
           "  2     *\r\n"
           "  3  %s\r\n\r\n"
           "Computing statistics for 75 seconds...\r\n"
           "            Source to Here   This Node/Link\r\n"
           "Hop  RTT    Lost/Sent = Pct  Lost/Sent = Pct  Address\r\n"
           "  0                                           DESKTOP-WINTERM [192.168.1.100]\r\n"
           "  1   1ms    0/ 100 =  0%%    0/ 100 =  0%%  router.home [192.168.1.1]\r\n"
           "  2  15ms    0/ 100 =  0%%    0/ 100 =  0%%  %s\r\n\r\n"
           "Trace complete.\r\n", arg, arg, arg);
}

static void cmd_ping(const char *arg) {
    if (!arg || !*arg) { printf("Usage: PING target_name [-n count]\r\n"); return; }
    /* parse -n flag */
    char host[256] = {0};
    int count = 4;
    const char *p = arg;
    if (strncmp(p, "-n ", 3) == 0 || strncmp(p, "/n ", 3) == 0) {
        count = atoi(p + 3);
        if (count <= 0) count = 4;
        p = strchr(p + 3, ' ');
        if (p) while (*p == ' ') p++;
        else p = "";
    }
    strncpy(host, p ? p : arg, 255);
    /* strip trailing spaces */
    int hl = strlen(host);
    while (hl > 0 && host[hl-1] == ' ') host[--hl] = 0;

    printf("\r\nPinging %s with 32 bytes of data:\r\n", host);
    for (int i = 0; i < count && i < 4; i++) {
        printf("Reply from 192.168.1.1: bytes=32 time<1ms TTL=128\r\n");
    }
    printf("\r\nPing statistics for %s:\r\n"
           "    Packets: Sent = %d, Received = %d, Lost = 0 (0%% loss),\r\n"
           "Approximate round trip times in milli-seconds:\r\n"
           "    Minimum = 0ms, Maximum = 0ms, Average = 0ms\r\n", host, count, count);
}

/* Directory stack for PUSHD/POPD */
#define PUSHD_STACK_SIZE 32
static char pushd_stack[PUSHD_STACK_SIZE][MAX_PATH];
static char pushd_win_stack[PUSHD_STACK_SIZE][MAX_PATH];
static int  pushd_top = 0;

static void cmd_pushd(const char *arg) {
    if (pushd_top >= PUSHD_STACK_SIZE) { printf("Directory stack overflow.\r\n"); return; }
    strncpy(pushd_stack[pushd_top], real_cwd, MAX_PATH);
    strncpy(pushd_win_stack[pushd_top], win_cwd, MAX_PATH);
    pushd_top++;
    if (arg && *arg) cmd_cd(arg);
}

static void cmd_popd(void) {
    if (pushd_top <= 0) { printf("Directory stack empty.\r\n"); return; }
    pushd_top--;
    strncpy(real_cwd, pushd_stack[pushd_top], MAX_PATH);
    strncpy(win_cwd,  pushd_win_stack[pushd_top], MAX_PATH);
    chdir(real_cwd);
}

static void cmd_print(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    FILE *f = fopen(real, "r");
    if (!f) { printf("PRINT: File not found - %s\r\n", arg); return; }
    printf("\r\n%s is currently being printed\r\n", arg);
    fclose(f);
}

static void cmd_prompt(const char *arg) {
    if (!arg || !*arg) {
        cmd_set("PROMPT=$P$G");
    } else {
        char kv[MAX_PATH + 8];
        snprintf(kv, sizeof(kv), "PROMPT=%s", arg);
        cmd_set(kv);
    }
}

static void cmd_reg(const char *rest) {
    if (!rest || !*rest) {
        printf("REG Operation [Parameter List]\r\n\r\n"
               "  Operation  [ QUERY   | ADD    | DELETE | COPY   |\r\n"
               "               SAVE    | LOAD   | UNLOAD | RESTORE|\r\n"
               "               COMPARE | EXPORT | IMPORT | FLAGS  ]\r\n"); return;
    }
    char sub[16] = {0};
    const char *p = rest;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 15) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;

    if (strcmp(sub, "QUERY") == 0) {
        printf("HKEY_LOCAL_MACHINE\r\n"
               "    (Default)    REG_SZ    (value not set)\r\n\r\n"
               "End of search: 1 match(es) found.\r\n");
    } else if (strcmp(sub, "ADD") == 0) {
        printf("The operation completed successfully.\r\n");
    } else if (strcmp(sub, "DELETE") == 0) {
        printf("Permanently delete the registry value %s (Yes/No)? ", p);
        fflush(stdout);
        char c[4]; fgets(c, sizeof(c), stdin);
        if (c[0]=='y'||c[0]=='Y') printf("The operation completed successfully.\r\n");
        else printf("Cancelled.\r\n");
    } else {
        printf("The operation completed successfully.\r\n");
    }
}

static void cmd_regedit(void) {
    printf("Registry Editor is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_replace(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[0]);
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[1]);
    FILE *in = fopen(src, "rb");
    if (!in) { printf("File not found - %s\r\n", args[0]); return; }
    FILE *out_f = fopen(dst, "wb");
    if (!out_f) { fclose(in); printf("Access is denied.\r\n"); return; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out_f);
    fclose(in); fclose(out_f);
    printf("1 file(s) replaced.\r\n");
}

static void cmd_robocopy(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    printf("\r\n"
           "-------------------------------------------------------------------------------\r\n"
           "   ROBOCOPY     ::     Robust File Copy for Windows\r\n"
           "-------------------------------------------------------------------------------\r\n\r\n"
           "  Started : %s\r\n"
           "   Source : %s\\\r\n"
           "     Dest : %s\\\r\n\r\n"
           "   Files : *.*\r\n\r\n"
           "  Options : /DCOPY:DA /COPY:DAT /R:1000000 /W:30\r\n\r\n"
           "------------------------------------------------------------------------------\r\n\r\n",
           "Thu May 07 12:00:00 2026", args[0], args[1]);
    /* Simulate copy */
    char src_real[MAX_PATH], dst_real[MAX_PATH];
    snprintf(src_real, sizeof(src_real), "%s/%s", real_cwd, args[0]);
    snprintf(dst_real, sizeof(dst_real), "%s/%s", real_cwd, args[1]);
    mkdir(dst_real, 0755);
    printf("   New Dir          1\t%s\\\r\n\r\n"
           "------------------------------------------------------------------------------\r\n\r\n"
           "               Total    Copied   Skipped  Mismatch    FAILED    Extras\r\n"
           "    Dirs :         1         1         0         0         0         0\r\n"
           "   Files :         0         0         0         0         0         0\r\n"
           "   Bytes :         0         0         0         0         0         0\r\n"
           "   Times :   0:00:00   0:00:00                       0:00:00   0:00:00\r\n\r\n"
           "   Ended : Thu May 07 12:00:01 2026\r\n\r\n", args[1]);
}

static void cmd_sc(const char *rest) {
    if (!rest || !*rest) {
        printf("DESCRIPTION:\r\n"
               "        SC is a command line program used for communicating with the\r\n"
               "        Service Control Manager and services.\r\n"); return;
    }
    char sub[32] = {0};
    const char *p = rest;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;

    if (strcmp(sub, "QUERY") == 0) {
        const char *svc = *p ? p : "wuauserv";
        printf("SERVICE_NAME: %s\r\n"
               "        TYPE               : 20  WIN32_SHARE_PROCESS\r\n"
               "        STATE              :  4  RUNNING\r\n"
               "                                (STOPPABLE, NOT_PAUSABLE, ACCEPTS_SHUTDOWN)\r\n"
               "        WIN32_EXIT_CODE    :  0  (0x0)\r\n"
               "        SERVICE_EXIT_CODE  :  0  (0x0)\r\n"
               "        CHECKPOINT         : 0x0\r\n"
               "        WAIT_HINT          : 0x0\r\n", svc);
    } else if (strcmp(sub, "START") == 0) {
        printf("[SC] StartService FAILED %s:\r\n\r\nThe service is already running.\r\n", p);
    } else if (strcmp(sub, "STOP") == 0) {
        printf("SERVICE_NAME: %s\r\n"
               "        TYPE               : 20  WIN32_SHARE_PROCESS\r\n"
               "        STATE              :  3  STOP_PENDING\r\n", p);
    } else {
        printf("[SC] OpenSCManager FAILED 5:\r\nAccess is denied.\r\n");
    }
}

static void cmd_schtasks(const char *rest) {
    if (!rest || !*rest || strcasecmp(rest, "/query") == 0) {
        printf("\r\nFolder: \\\r\n"
               "TaskName                                 Next Run Time          Status\r\n"
               "======================================== ====================== ===============\r\n"
               "\\MicrosoftEdgeUpdateTaskMachineCore      5/7/2026 3:00:00 PM    Ready\r\n"
               "\\MicrosoftEdgeUpdateTaskMachineUA        5/7/2026 12:26:00 PM   Ready\r\n"
               "\\OneDrive Standalone Update Task         5/7/2026 12:10:14 AM   Ready\r\n");
    } else {
        printf("SUCCESS: The scheduled task has been successfully processed.\r\n");
    }
}

static void cmd_setlocal(void) {
    /* no-op in single-process emulator */
    printf("SETLOCAL environment captured.\r\n");
}

static void cmd_endlocal(void) {
    printf("ENDLOCAL environment restored.\r\n");
}

static void cmd_shift(const char *arg) {
    (void)arg;
    printf("SHIFT: batch parameter shifting (no-op outside batch context).\r\n");
}

static void cmd_shutdown(const char *arg) {
    int do_shutdown = 0, do_restart = 0, do_logoff = 0;
    if (arg) {
        if (strstr(arg, "/s") || strstr(arg, "-s")) do_shutdown = 1;
        if (strstr(arg, "/r") || strstr(arg, "-r")) do_restart  = 1;
        if (strstr(arg, "/l") || strstr(arg, "-l")) do_logoff   = 1;
        if (strstr(arg, "/a") || strstr(arg, "-a")) { printf("The shutdown has been aborted.\r\n"); return; }
        if (strstr(arg, "/?") || strstr(arg, "-?") || !*arg) {
            printf("Usage: SHUTDOWN [/i | /l | /s | /r | /g | /a | /p | /h | /e]\r\n"
                   "  /l  Log off\r\n  /s  Shut down\r\n  /r  Restart\r\n"
                   "  /a  Abort a system shutdown\r\n"); return;
        }
    }
    if (do_logoff)       printf("Logging off...\r\n");
    else if (do_restart) printf("Restarting...\r\n");
    else if (do_shutdown) {
        printf("Shutting down...\r\n");
        exit(0);
    } else {
        printf("Usage: SHUTDOWN /s /r /l /a /?\r\n");
    }
}

static void cmd_sort(const char *args_str) {
    /* Read from file if given, else prompt */
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc > 0) {
        char real[MAX_PATH];
        snprintf(real, sizeof(real), "%s/%s", real_cwd, args[0]);
        FILE *f = fopen(real, "r");
        if (!f) { printf("The system cannot find the file specified.\r\n"); return; }
        char lines[4096][256]; int cnt = 0;
        while (cnt < 4096 && fgets(lines[cnt], 256, f)) {
            lines[cnt][strcspn(lines[cnt],"\r\n")] = 0;
            cnt++;
        }
        fclose(f);
        /* bubble sort */
        for (int i = 0; i < cnt-1; i++)
            for (int j = i+1; j < cnt; j++)
                if (strcmp(lines[i], lines[j]) > 0) {
                    char tmp[256]; strcpy(tmp, lines[i]); strcpy(lines[i], lines[j]); strcpy(lines[j], tmp);
                }
        for (int i = 0; i < cnt; i++) printf("%s\r\n", lines[i]);
    } else {
        printf("Enter text to sort (Ctrl+Z/Ctrl+D to end):\r\n");
        char lines[4096][256]; int cnt = 0;
        while (cnt < 4096 && fgets(lines[cnt], 256, stdin)) {
            lines[cnt][strcspn(lines[cnt],"\r\n")] = 0;
            cnt++;
        }
        for (int i = 0; i < cnt-1; i++)
            for (int j = i+1; j < cnt; j++)
                if (strcmp(lines[i], lines[j]) > 0) {
                    char tmp[256]; strcpy(tmp, lines[i]); strcpy(lines[i], lines[j]); strcpy(lines[j], tmp);
                }
        for (int i = 0; i < cnt; i++) printf("%s\r\n", lines[i]);
    }
}

static void cmd_start(const char *arg) {
    if (!arg || !*arg) { printf("Opens a new command window.\r\n"); return; }
    printf("'%s' cannot be started from this terminal emulator.\r\n", arg);
}

static void cmd_subst(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc == 0) {
        printf("(No substitutions active)\r\n"); return;
    }
    if (argc >= 2) {
        printf("%s\\: => %s\r\n", args[0], args[1]);
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_systeminfo(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date[64];
    strftime(date, sizeof(date), "%m/%d/%Y, %I:%M:%S %p", tm);
    printf("Host Name:                 DESKTOP-WINTERM\r\n"
           "OS Name:                   Microsoft Windows 10 Pro\r\n"
           "OS Version:                10.0.19045 N/A Build 19045\r\n"
           "OS Manufacturer:           Microsoft Corporation\r\n"
           "OS Configuration:          Standalone Workstation\r\n"
           "OS Build Type:             Multiprocessor Free\r\n"
           "Registered Owner:          N/A\r\n"
           "Registered Organization:   N/A\r\n"
           "Product ID:                00330-80000-00000-AA832\r\n"
           "Original Install Date:     1/1/2024, 12:00:00 AM\r\n"
           "System Boot Time:          %s\r\n"
           "System Manufacturer:       LENOVO\r\n"
           "System Model:              81BK\r\n"
           "System Type:               x64-based PC\r\n"
           "Processor(s):              1 Processor(s) Installed.\r\n"
           "                           [01]: AMD64 Family 23 Model 113 Stepping 0\r\n"
           "                                 AuthenticAMD ~3600 Mhz\r\n"
           "BIOS Version:              LENOVO DWCN45WW, 10/14/2022\r\n"
           "Windows Directory:         C:\\Windows\r\n"
           "System Directory:          C:\\Windows\\system32\r\n"
           "Boot Device:               \\Device\\HarddiskVolume1\r\n"
           "System Locale:             en-us;English (United States)\r\n"
           "Input Locale:              en-us;English (United States)\r\n"
           "Time Zone:                 (UTC-05:00) Eastern Time (US & Canada)\r\n"
           "Total Physical Memory:     16,384 MB\r\n"
           "Available Physical Memory: 8,192 MB\r\n"
           "Virtual Memory: Max Size:  18,874 MB\r\n"
           "Virtual Memory: Available: 9,437 MB\r\n"
           "Virtual Memory: In Use:    9,437 MB\r\n"
           "Page File Location(s):     C:\\pagefile.sys\r\n"
           "Domain:                    WORKGROUP\r\n"
           "Logon Server:              \\\\DESKTOP-WINTERM\r\n"
           "Hotfix(s):                 10 Hotfix(s) Installed.\r\n"
           "Network Card(s):           1 NIC(s) Installed.\r\n"
           "                           [01]: Intel(R) Wi-Fi 6 AX200 160MHz\r\n"
           "                                 Connection Name: Wi-Fi\r\n"
           "                                 DHCP Enabled:    Yes\r\n"
           "                                 DHCP Server:     192.168.1.1\r\n"
           "                                 IP address(es)\r\n"
           "                                 [01]: 192.168.1.100\r\n"
           "Hyper-V Requirements:      VM Monitor Mode Extensions: Yes\r\n", date);
}

static void cmd_takeown(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    printf("SUCCESS: The file (or folder): \"%s\" now owned by the administrators group.\r\n", arg);
}

static void cmd_taskkill(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    /* parse /IM name or /PID number */
    if (strstr(arg, "/IM") || strstr(arg, "/im")) {
        const char *p = strstr(arg, "/IM");
        if (!p) p = strstr(arg, "/im");
        p += 3;
        while (*p == ' ') p++;
        printf("SUCCESS: Sent termination signal to the process \"%s\".\r\n", p);
    } else if (strstr(arg, "/PID") || strstr(arg, "/pid")) {
        const char *p = strstr(arg, "/PID");
        if (!p) p = strstr(arg, "/pid");
        p += 4;
        while (*p == ' ') p++;
        printf("SUCCESS: The process with PID %s has been terminated.\r\n", p);
    } else {
        printf("ERROR: Invalid argument/option - '%s'.\r\n", arg);
    }
}

static void cmd_tasklist(const char *arg) {
    (void)arg;
    printf("\r\nImage Name                     PID Session Name        Session#    Mem Usage\r\n"
           "========================= ======== ================ =========== ============\r\n"
           "System Idle Process              0 Services                   0         8 K\r\n"
           "System                           4 Services                   0     1,680 K\r\n"
           "Registry                        96 Services                   0    43,452 K\r\n"
           "smss.exe                       392 Services                   0     1,080 K\r\n"
           "csrss.exe                      600 Services                   0     5,468 K\r\n"
           "wininit.exe                    700 Services                   0     5,564 K\r\n"
           "services.exe                   744 Services                   0     8,096 K\r\n"
           "lsass.exe                      752 Services                   0    15,404 K\r\n"
           "svchost.exe                    936 Services                   0    24,712 K\r\n"
           "svchost.exe                    984 Services                   0    15,076 K\r\n"
           "svchost.exe                   1024 Services                   0    58,412 K\r\n"
           "svchost.exe                   1048 Services                   0    20,184 K\r\n"
           "svchost.exe                   1340 Services                   0    36,680 K\r\n"
           "svchost.exe                   1420 Services                   0    19,528 K\r\n"
           "spoolsv.exe                   1640 Services                   0    12,544 K\r\n"
           "explorer.exe                  3344 Console                    1   119,984 K\r\n"
           "SearchIndexer.exe             4208 Services                   0    60,248 K\r\n"
           "cmd.exe                       5168 Console                    1     4,456 K\r\n"
           "conhost.exe                   5192 Console                    1    12,480 K\r\n"
           "winterm.exe                   5200 Console                    1     2,024 K\r\n");
}

static void cmd_timeout(const char *arg) {
    int secs = 5;
    if (arg && *arg) {
        /* skip /T flag */
        const char *p = arg;
        if (strncasecmp(p, "/t ", 3) == 0 || strncasecmp(p, "-t ", 3) == 0) p += 3;
        secs = atoi(p);
        if (secs < 0) secs = 0;
    }
    printf("Waiting for %d seconds, press a key to continue ...\r\n", secs);
    /* We simulate without actually sleeping */
}

static void cmd_title(const char *arg) {
    if (arg && *arg) printf("\033]0;%s\007", arg); /* set terminal title via ANSI OSC */
    fflush(stdout);
}

static void cmd_tracert(const char *arg) {
    if (!arg || !*arg) { printf("Usage: TRACERT target_name\r\n"); return; }
    printf("\r\nTracing route to %s over a maximum of 30 hops\r\n\r\n"
           "  1    <1 ms    <1 ms    <1 ms  router.home [192.168.1.1]\r\n"
           "  2     5 ms     4 ms     5 ms  10.0.0.1\r\n"
           "  3    12 ms    11 ms    12 ms  100.64.0.1\r\n"
           "  4    15 ms    14 ms    15 ms  %s\r\n\r\n"
           "Trace complete.\r\n", arg, arg);
}

static void cmd_vol(void) {
    printf(" Volume in drive C is WINDOWS\r\n"
           " Volume Serial Number is 1A2B-3C4D\r\n");
}

static void cmd_where(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    /* Check for fake System32 EXEs */
    static const char *sys32_exes[] = {
        "cmd","notepad","calc","mspaint","explorer","taskmgr","regedit",
        "powershell","msiexec","regsvr32","svchost","lsass","winlogon",
        "services","spoolsv","wininit","csrss","smss","conhost",
        "wmic","net","netstat","ipconfig","ping","tracert","nslookup",
        "attrib","chkdsk","diskpart","dism","schtasks","sc","robocopy",
        "xcopy","shutdown","systeminfo","tasklist","taskkill",
        "driverquery","gpresult","whoami","icacls","takeown",NULL
    };
    char lower[128] = {0};
    int li = 0;
    const char *p = arg;
    while (*p && li < 127) lower[li++] = tolower((unsigned char)*p++);
    /* strip .exe */
    char *dot = strrchr(lower, '.');
    if (dot && strcasecmp(dot, ".exe") == 0) *dot = 0;

    for (int i = 0; sys32_exes[i]; i++) {
        if (strcmp(lower, sys32_exes[i]) == 0) {
            printf("C:\\Windows\\System32\\%s.exe\r\n", arg);
            return;
        }
    }
    printf("INFO: Could not find files for the given pattern(s).\r\n");
}

static void cmd_whoami(const char *arg) {
    int priv = arg && (strcasecmp(arg, "/priv") == 0 || strstr(arg, "/priv") != NULL);
    int groups = arg && strstr(arg, "/groups") != NULL;
    int all = arg && strcasecmp(arg, "/all") == 0;

    if (!arg || !*arg || (!priv && !groups && !all)) {
        printf("desktop-winterm\\administrator\r\n");
        return;
    }
    if (groups || all) {
        printf("\r\nGROUP INFORMATION\r\n"
               "-----------------\r\n\r\n"
               "Group Name                             Type             SID          Attributes\r\n"
               "====================================== ================ ============ ==========================\r\n"
               "Everyone                               Well-known group S-1-1-0      Mandatory group, Enabled\r\n"
               "BUILTIN\\Administrators                 Alias            S-1-5-32-544 Mandatory group, Enabled\r\n"
               "BUILTIN\\Users                          Alias            S-1-5-32-545 Mandatory group, Enabled\r\n"
               "NT AUTHORITY\\INTERACTIVE               Well-known group S-1-5-4      Mandatory group, Enabled\r\n"
               "NT AUTHORITY\\Authenticated Users       Well-known group S-1-5-11     Mandatory group, Enabled\r\n"
               "NT AUTHORITY\\This Organization         Well-known group S-1-5-15     Mandatory group, Enabled\r\n"
               "NT AUTHORITY\\Local account             Well-known group S-1-5-113    Mandatory group, Enabled\r\n"
               "Mandatory Label\\High Mandatory Level   Label            S-1-16-12288 Mandatory group, Enabled\r\n");
    }
    if (priv || all) {
        printf("\r\nPRIVILEGE INFORMATION\r\n"
               "---------------------\r\n\r\n"
               "Privilege Name                  Description                               State\r\n"
               "=============================== ========================================= ========\r\n"
               "SeIncreaseQuotaPrivilege        Adjust memory quotas for a process        Disabled\r\n"
               "SeSecurityPrivilege             Manage auditing and security log           Disabled\r\n"
               "SeTakeOwnershipPrivilege        Take ownership of files or objects        Disabled\r\n"
               "SeLoadDriverPrivilege           Load and unload device drivers            Disabled\r\n"
               "SeSystemProfilePrivilege        Profile system performance                Disabled\r\n"
               "SeSystemtimePrivilege           Change the system time                    Disabled\r\n"
               "SeProfileSingleProcessPrivilege Profile single process                    Disabled\r\n"
               "SeIncreaseBasePriorityPrivilege Increase scheduling priority              Disabled\r\n"
               "SeCreatePagefilePrivilege       Create a pagefile                         Disabled\r\n"
               "SeBackupPrivilege               Back up files and directories             Disabled\r\n"
               "SeRestorePrivilege              Restore files and directories             Disabled\r\n"
               "SeShutdownPrivilege             Shut down the system                      Disabled\r\n"
               "SeDebugPrivilege                Debug programs                            Enabled\r\n"
               "SeSystemEnvironmentPrivilege    Modify firmware environment values        Disabled\r\n"
               "SeChangeNotifyPrivilege         Bypass traverse checking                  Enabled\r\n"
               "SeRemoteShutdownPrivilege       Force shutdown from a remote system       Disabled\r\n"
               "SeUndockPrivilege               Remove computer from docking station      Disabled\r\n"
               "SeManageVolumePrivilege         Perform volume maintenance tasks          Disabled\r\n"
               "SeImpersonatePrivilege          Impersonate a client after auth           Enabled\r\n"
               "SeCreateGlobalPrivilege         Create global objects                     Enabled\r\n"
               "SeIncreaseWorkingSetPrivilege   Increase a process working set            Disabled\r\n"
               "SeTimeZonePrivilege             Change the time zone                      Disabled\r\n"
               "SeCreateSymbolicLinkPrivilege   Create symbolic links                     Enabled\r\n"
               "SeDelegateSessionUserImpersonatePrivilege Impersonate other session      Disabled\r\n");
    }
}

static void cmd_winver(void) {
    printf("Microsoft Windows\r\n"
           "Version 10.0.19045.4291\r\n"
           "(c) Microsoft Corporation. All rights reserved.\r\n");
}

static void cmd_wmic(const char *rest) {
    if (!rest || !*rest) {
        printf("wmic:root\\cli>");
        fflush(stdout);
        char line[MAX_INPUT];
        if (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line,"\r\n")] = 0;
            /* re-enter as WMIC query */
            cmd_wmic(line);
        }
        return;
    }
    char upper[MAX_INPUT];
    for (int i = 0; rest[i] && i < MAX_INPUT-1; i++) upper[i] = toupper((unsigned char)rest[i]);
    upper[strlen(rest)] = 0;

    if (strstr(upper, "CPU") && strstr(upper, "GET")) {
        printf("Caption                                    DeviceID  MaxClockSpeed  Name\r\n"
               "AMD Ryzen 5 3600 6-Core Processor          CPU0      3600           AMD Ryzen 5 3600\r\n");
    } else if (strstr(upper, "OS") && strstr(upper, "GET")) {
        printf("Caption                              OSArchitecture  Version\r\n"
               "Microsoft Windows 10 Pro             64-bit          10.0.19045\r\n");
    } else if (strstr(upper, "BIOS") && strstr(upper, "GET")) {
        printf("Manufacturer  Name           SerialNumber  Version\r\n"
               "LENOVO        DWCN45WW(V1.0) PF2ABCDE      LENOVO - 1\r\n");
    } else if (strstr(upper, "MEMORYCHIP") || strstr(upper, "PHYSICALMEMORY")) {
        printf("BankLabel  Capacity    DeviceLocator  Speed\r\n"
               "BANK 0     8589934592  ChannelA-DIMM0 3200\r\n"
               "BANK 1     8589934592  ChannelB-DIMM0 3200\r\n");
    } else if (strstr(upper, "DISKDRIVE") && strstr(upper, "GET")) {
        printf("DeviceID            Model                   Size\r\n"
               "\\\\.\\PHYSICALDRIVE0  Samsung SSD 870 EVO     128035676160\r\n");
    } else if (strstr(upper, "NIC") || strstr(upper, "NICCONFIG")) {
        printf("Caption                          IPAddress         MACAddress\r\n"
               "Intel Wi-Fi 6 AX200              192.168.1.100     AA:BB:CC:DD:EE:FF\r\n");
    } else if (strstr(upper, "PROCESS") && strstr(upper, "GET")) {
        printf("Name                 ProcessId  ParentProcessId\r\n"
               "System Idle Process  0          0\r\n"
               "System               4          0\r\n"
               "smss.exe             392        4\r\n"
               "csrss.exe            600        392\r\n"
               "winlogon.exe         700        392\r\n"
               "explorer.exe         3344       700\r\n"
               "cmd.exe              5168       3344\r\n"
               "winterm.exe          5200       5168\r\n");
    } else {
        printf("No Instance(s) Available.\r\n");
    }
}

static void cmd_xcopy(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", real_cwd, args[0]);
    snprintf(dst, sizeof(dst), "%s/%s", real_cwd, args[1]);

    struct stat st;
    if (stat(src, &st) != 0) { printf("File not found - %s\r\n", args[0]); return; }

    if (S_ISDIR(st.st_mode)) {
        mkdir(dst, 0755);
        printf("Does %s specify a file name\r\nor directory name on the target\r\n(F = file, D = directory)? D\r\n", args[1]);
        printf("0 File(s) copied\r\n");
    } else {
        FILE *in = fopen(src, "rb");
        if (!in) { printf("File not found - %s\r\n", args[0]); return; }
        /* If dst is a dir, append filename */
        char dst_file[MAX_PATH];
        if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(dst_file, sizeof(dst_file), "%s/%s", dst, args[0]);
        else
            strncpy(dst_file, dst, MAX_PATH);
        FILE *out_f = fopen(dst_file, "wb");
        if (!out_f) { fclose(in); printf("Access is denied.\r\n"); return; }
        char buf[8192]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out_f);
        fclose(in); fclose(out_f);
        printf("%s\r\n1 File(s) copied\r\n", args[0]);
    }
}

/* ── Missing commands ────────────────────────────────────────────────────── */

static void cmd_arp(const char *arg) {
    (void)arg;
    printf("\r\nInterface: 192.168.1.100 --- 0x4\r\n"
           "  Internet Address      Physical Address      Type\r\n"
           "  192.168.1.1           aa-bb-cc-dd-ee-ff     dynamic\r\n"
           "  192.168.1.255         ff-ff-ff-ff-ff-ff     static\r\n"
           "  224.0.0.22            01-00-5e-00-00-16     static\r\n"
           "  255.255.255.255       ff-ff-ff-ff-ff-ff     static\r\n\r\n");
}

static void cmd_at(const char *arg) {
    (void)arg;
    printf("The AT command has been deprecated. Please use SCHTASKS.\r\n");
}

static void cmd_bcdedit(const char *arg) {
    (void)arg;
    printf("Windows Boot Manager\r\n"
           "--------------------\r\n"
           "identifier              {bootmgr}\r\n"
           "device                  partition=\\Device\\HarddiskVolume1\r\n"
           "path                    \\EFI\\Microsoft\\Boot\\bootmgfw.efi\r\n"
           "description             Windows Boot Manager\r\n"
           "locale                  en-US\r\n"
           "inherit                 {globalsettings}\r\n"
           "default                 {current}\r\n"
           "resumeobject            {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\r\n"
           "displayorder            {current}\r\n"
           "toolsdisplayorder       {memdiag}\r\n"
           "timeout                 30\r\n\r\n"
           "Windows Boot Loader\r\n"
           "-------------------\r\n"
           "identifier              {current}\r\n"
           "device                  partition=C:\r\n"
           "path                    \\Windows\\system32\\winload.efi\r\n"
           "description             Windows 10\r\n"
           "locale                  en-US\r\n"
           "inherit                 {bootloadersettings}\r\n"
           "recoverysequence        {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\r\n"
           "displaymessageoverride  Recovery\r\n"
           "recoveryenabled         Yes\r\n"
           "isolatedcontext         Yes\r\n"
           "allowedinmemorysettings 0x15000075\r\n"
           "osdevice               partition=C:\r\n"
           "systemroot              \\Windows\r\n"
           "resumeobject            {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\r\n"
           "nx                      OptIn\r\n"
           "bootmenupolicy         Standard\r\n");
}

static void cmd_bootrec(const char *arg) {
    (void)arg;
    printf("The operation completed successfully.\r\n"
           "Scanned Windows installations: 1\r\n"
           "Total identified Windows installations: 1\r\n"
           "[C:\\Windows]\r\n");
}

static void cmd_break(const char *arg) {
    (void)arg;
    printf("BREAK is off.\r\n");
}

static void cmd_cipher(const char *arg) {
    if (!arg || !*arg) {
        printf(" Listing %s\\\r\n\r\n"
               "New files added to this directory will not be encrypted.\r\n\r\n"
               "U %s\\\r\n", win_cwd, win_cwd);
        return;
    }
    if (strstr(arg, "/e") || strstr(arg, "/E")) {
        printf("Encrypting files in %s\\\r\n"
               "Adding %s [OK]\r\n"
               "1 file(s) [or directorie(s)] within 1 directorie(s) were encrypted.\r\n", win_cwd, arg);
    } else if (strstr(arg, "/d") || strstr(arg, "/D")) {
        printf("Decrypting files in %s\\\r\n"
               "Removing %s [OK]\r\n"
               "1 file(s) [or directorie(s)] within 1 directorie(s) were decrypted.\r\n", win_cwd, arg);
    } else {
        printf("U %s\r\n", arg);
    }
}

static void cmd_clip(void) {
    /* Read stdin and pretend to put it on clipboard */
    printf("(Reading from stdin - Ctrl+D to end)\r\n");
    char buf[MAX_INPUT];
    while (fgets(buf, sizeof(buf), stdin)) { /* consume */ }
    printf("Content copied to clipboard (simulated).\r\n");
}

static void cmd_choice(const char *arg) {
    /* CHOICE [/C choices] [/N] [/T timeout /D default] [/M message] */
    const char *choices = "YN";
    const char *message = NULL;
    if (arg && *arg) {
        const char *c = strstr(arg, "/C");
        if (!c) c = strstr(arg, "/c");
        if (c) { c += 2; while (*c == ' ') c++; choices = c; }
        const char *m = strstr(arg, "/M");
        if (!m) m = strstr(arg, "/m");
        if (m) { m += 2; while (*m == ' ') m++; message = m; }
    }
    if (message) printf("%s", message);
    else printf("Choose: ");
    /* Print [Y,N]? style */
    printf("[");
    for (int i = 0; choices[i] && choices[i] != ' '; i++) {
        if (i) printf(",");
        printf("%c", toupper((unsigned char)choices[i]));
    }
    printf("]? ");
    fflush(stdout);
    char c[4];
    if (fgets(c, sizeof(c), stdin)) {
        printf("\r\n");
        /* ERRORLEVEL set to position of choice */
        char uc = toupper((unsigned char)c[0]);
        int pos = 1;
        for (int i = 0; choices[i] && choices[i] != ' '; i++) {
            if (toupper((unsigned char)choices[i]) == uc) { pos = i+1; break; }
        }
        (void)pos; /* ERRORLEVEL would be set here in real CMD */
    }
}

static void cmd_defrag(const char *arg) {
    const char *drive = (arg && *arg) ? arg : "C:";
    printf("Microsoft Drive Optimizer\r\n"
           "Copyright (c) Microsoft Corp.\r\n\r\n"
           "Invoking optimization on %s ...\r\n"
           "Stage 1: Analyzing volume ...\r\n"
           "  100%% complete\r\n"
           "Stage 2: Defragmenting volume ...\r\n"
           "  100%% complete\r\n"
           "Stage 3: Compacting volume (if requested) ...\r\n"
           "  100%% complete\r\n\r\n"
           "The operation completed successfully.\r\n", drive);
}

static void cmd_expand(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 1) { printf("The syntax of the command is incorrect.\r\n"); return; }
    printf("Microsoft (R) File Expansion Utility  Version 5.2.3790.0\r\n"
           "Copyright (C) Microsoft Corp 1990-2003. All rights reserved.\r\n\r\n");
    if (argc >= 2)
        printf("Adding %s to %s.\r\n", args[0], args[1]);
    else
        printf("Expanding %s...\r\n", args[0]);
    printf("Expanding file completed.\r\n");
}

static void cmd_fsutil(const char *rest) {
    if (!rest || !*rest) {
        printf("---- Commands Supported ----\r\n\r\n"
               "behavior          Control file system behavior\r\n"
               "dirty             Manage volume dirty bit\r\n"
               "file              File specific commands\r\n"
               "hardlink          Hardlink management\r\n"
               "objectid          Object ID management\r\n"
               "quota             Quota management\r\n"
               "repair            Self healing management\r\n"
               "reparsepoint      Reparse point management\r\n"
               "resource          Transactional Resource Manager management\r\n"
               "sparse            Sparse file control\r\n"
               "transaction       Transaction management\r\n"
               "usn               USN management\r\n"
               "volume            Volume management\r\n"
               "wim               Transparent wim hosting management\r\n");
        return;
    }
    char sub[32] = {0};
    const char *p = rest;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;

    if (strcmp(sub, "DIRTY") == 0) {
        printf("Volume - C:\r\nThe dirty bit is NOT set.\r\n");
    } else if (strcmp(sub, "VOLUME") == 0) {
        printf("Volume Name : WINDOWS\r\nSerial Number : 1A2B3C4D\r\nMax Component Length : 255\r\nFile System Name : NTFS\r\n");
    } else if (strcmp(sub, "FILE") == 0) {
        printf("---- %s Commands Supported ----\r\ncreateNew, findbySID, queryEA, setEA, setShortName,\r\nqueryInfo, querySecurity, setSecurity, setZeroData,\r\nqueryExtents, setValidData, queryFileNameById,\r\nqueryOptimizeMetadata, queryInformation\r\n", sub);
    } else {
        printf("fsutil %s - operation completed.\r\n", sub);
    }
}

/* GOTO support: run a batch file from a label */
/* Note: GOTO is handled inline in dispatch for batch mode;
   for interactive use we just report not applicable. */
static void cmd_goto(const char *label) {
    (void)label;
    printf("GOTO: Not applicable outside a batch context.\r\n");
}

static void cmd_more(const char *arg) {
    FILE *f = NULL;
    if (arg && *arg) {
        char real[MAX_PATH];
        snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
        f = fopen(real, "r");
        if (!f) { printf("File not found - %s\r\n", arg); return; }
    }
    int line_count = 0;
    char buf[MAX_INPUT];
    while (fgets(buf, sizeof(buf), f ? f : stdin)) {
        printf("%s", buf);
        line_count++;
        if (line_count % 24 == 0) {
            printf("-- More --");
            fflush(stdout);
            char c[4]; fgets(c, sizeof(c), stdin);
        }
    }
    if (f) fclose(f);
}

static void cmd_msiexec(const char *arg) {
    if (!arg || !*arg) {
        printf("Windows ® Installer. V 5.0.19041.3636\r\n\r\n"
               "msiexec /Option <Required Parameter> [Optional Parameter]\r\n\r\n"
               "Install Options\r\n"
               "\t</package | /i> <Product.msi>\r\n"
               "\t\tInstalls or configures a product\r\n"
               "\t/a <Product.msi>\r\n"
               "\t\tAdministrative install - Installs a product on the network\r\n"
               "\t/j<u|m> <Product.msi> [/t <Transform List>] [/g <Language ID>]\r\n"
               "\t\tAdvertises a product - m to all users, u to current user\r\n"
               "\t</uninstall | /x> <Product.msi | ProductCode>\r\n"
               "\t\tUninstalls the product\r\n");
        return;
    }
    printf("Installation complete (simulated).\r\n");
}

static void cmd_msconfig(void) {
    printf("System Configuration is a GUI application and cannot run in this terminal.\r\n"
           "Use MSCONFIG.EXE from the Run dialog.\r\n");
}

static void cmd_netsh(const char *rest) {
    if (!rest || !*rest) {
        printf("netsh>");
        fflush(stdout);
        char line[MAX_INPUT];
        if (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line,"\r\n")] = 0;
            if (strcasecmp(line, "exit") == 0 || strcasecmp(line, "quit") == 0)
                printf("Leaving netsh...\r\n");
            else
                printf("The following command was not found: %s.\r\n", line);
        }
        return;
    }
    char upper[MAX_INPUT];
    for (int i = 0; rest[i] && i < MAX_INPUT-1; i++) upper[i] = toupper((unsigned char)rest[i]);
    upper[strlen(rest)] = 0;

    if (strstr(upper, "WLAN") && strstr(upper, "SHOW")) {
        printf("There are 1 interface(s) on the system:\r\n\r\n"
               "    Name                   : Wi-Fi\r\n"
               "    Description            : Intel(R) Wi-Fi 6 AX200 160MHz\r\n"
               "    GUID                   : xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\r\n"
               "    Physical address       : aa:bb:cc:dd:ee:ff\r\n"
               "    Interface type         : Primary STA\r\n"
               "    State                  : connected\r\n"
               "    SSID                   : MyNetwork\r\n"
               "    BSSID                  : 11:22:33:44:55:66\r\n"
               "    Network type           : Infrastructure\r\n"
               "    Radio type             : 802.11ac\r\n"
               "    Authentication         : WPA2-Personal\r\n"
               "    Cipher                 : CCMP \r\n"
               "    Connection mode        : Auto Connect\r\n"
               "    Channel                : 6\r\n"
               "    Receive rate (Mbps)    : 400\r\n"
               "    Transmit rate (Mbps)   : 400\r\n"
               "    Signal                 : 90%%\r\n"
               "    Profile                : MyNetwork\r\n");
    } else if (strstr(upper, "INTERFACE") && strstr(upper, "SHOW")) {
        printf("Admin State    State          Type             Interface Name\r\n"
               "-------------------------------------------------------------------------\r\n"
               "Enabled        Connected      Dedicated        Ethernet\r\n"
               "Enabled        Connected      Dedicated        Wi-Fi\r\n"
               "Enabled        Connected      Loopback         Loopback Pseudo-Interface 1\r\n");
    } else if (strstr(upper, "FIREWALL")) {
        printf("Ok.\r\n");
    } else {
        printf("The following command was not found: %s.\r\n", rest);
    }
}

static void cmd_nslookup(const char *arg) {
    if (!arg || !*arg) {
        printf("Default Server:  router.home\r\nAddress:  192.168.1.1\r\n\r\n> ");
        fflush(stdout);
        char line[MAX_INPUT];
        if (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line,"\r\n")] = 0;
            if (*line && strcasecmp(line, "exit") != 0) {
                printf("Server:  router.home\r\nAddress:  192.168.1.1\r\n\r\n"
                       "Non-authoritative answer:\r\n"
                       "Name:    %s\r\nAddress:  93.184.216.34\r\n", line);
            }
        }
        return;
    }
    printf("Server:  router.home\r\nAddress:  192.168.1.1\r\n\r\n"
           "Non-authoritative answer:\r\n"
           "Name:    %s\r\nAddress:  93.184.216.34\r\n", arg);
}

static void cmd_pktmon(const char *arg) {
    (void)arg;
    printf("Pktmon - Windows Packet Monitor\r\n"
           "Usage: pktmon [command] [options]\r\n\r\n"
           "COMMANDS:\r\n"
           "  start       Start packet capture.\r\n"
           "  stop        Stop packet capture.\r\n"
           "  status      Display running status.\r\n"
           "  reset       Reset counters.\r\n"
           "  list        List active filters.\r\n"
           "  filter      Manage packet filters.\r\n"
           "  comp        Manage registered components.\r\n"
           "  counters    Display per-component counters.\r\n"
           "  etl         Work with ETL files.\r\n\r\n"
           "(Packet capture simulated - no packets collected)\r\n");
}

static void cmd_regsvr32(const char *arg) {
    if (!arg || !*arg) {
        printf("RegSvr32: No DLL name specified.\r\nUsage: REGSVR32 [/U] [/S] [/N] [/I[:cmdline]] dllname\r\n");
        return;
    }
    int unreg = strstr(arg, "/U") || strstr(arg, "/u");
    /* strip flags to get dll name */
    const char *p = arg;
    while (*p == '/' || isalpha((unsigned char)*p) || *p == ' ') {
        if (*p == ' ' && *(p+1) != '/') { p++; break; }
        p++;
    }
    if (!*p) p = arg;
    if (unreg)
        printf("DllUnregisterServer in %s succeeded.\r\n", p);
    else
        printf("DllRegisterServer in %s succeeded.\r\n", p);
}

static void cmd_route(const char *rest) {
    if (!rest || !*rest || strcasecmp(rest, "print") == 0) {
        printf("\r\n===========================================================================\r\n"
               "Interface List\r\n"
               " 4...aa bb cc dd ee ff ......Intel(R) Wi-Fi 6 AX200 160MHz\r\n"
               " 1...........................Software Loopback Interface 1\r\n"
               "===========================================================================\r\n\r\n"
               "IPv4 Route Table\r\n"
               "===========================================================================\r\n"
               "Active Routes:\r\n"
               "Network Destination        Netmask          Gateway       Interface  Metric\r\n"
               "          0.0.0.0          0.0.0.0      192.168.1.1    192.168.1.100     35\r\n"
               "        127.0.0.0        255.0.0.0         On-link        127.0.0.1    331\r\n"
               "        127.0.0.1  255.255.255.255         On-link        127.0.0.1    331\r\n"
               "  127.255.255.255  255.255.255.255         On-link        127.0.0.1    331\r\n"
               "      192.168.1.0    255.255.255.0         On-link    192.168.1.100    291\r\n"
               "    192.168.1.100  255.255.255.255         On-link    192.168.1.100    291\r\n"
               "    192.168.1.255  255.255.255.255         On-link    192.168.1.100    291\r\n"
               "        224.0.0.0        240.0.0.0         On-link        127.0.0.1    331\r\n"
               "  255.255.255.255  255.255.255.255         On-link        127.0.0.1    331\r\n"
               "===========================================================================\r\n"
               "Persistent Routes:\r\n  None\r\n");
    } else if (strncasecmp(rest, "add", 3) == 0) {
        printf("OK!\r\n");
    } else if (strncasecmp(rest, "delete", 6) == 0 || strncasecmp(rest, "del", 3) == 0) {
        printf("OK!\r\n");
    } else if (strncasecmp(rest, "change", 6) == 0) {
        printf("OK!\r\n");
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_runas(const char *arg) {
    if (!arg || !*arg) {
        printf("RUNAS USAGE:\r\n\r\n"
               "RUNAS [ [/noprofile | /profile] [/env] [/savecred | /netonly] ]\r\n"
               "        /user:<UserName> program\r\n"); return;
    }
    const char *user = strstr(arg, "/user:");
    if (user) {
        user += 6;
        printf("Enter the password for %s: ", user);
        fflush(stdout);
        char pw[128]; fgets(pw, sizeof(pw), stdin);
        printf("Attempting to start program as %s (simulated).\r\n", user);
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_sfc(const char *arg) {
    (void)arg;
    printf("Beginning system scan.  This process will take some time.\r\n\r\n"
           "Beginning verification phase of system scan.\r\n"
           "Verification 100%% complete.\r\n\r\n"
           "Windows Resource Protection did not find any integrity violations.\r\n");
}

static void cmd_verify(const char *arg) {
    static int verify_on = 0;
    if (!arg || !*arg) {
        printf("VERIFY is %s.\r\n", verify_on ? "on" : "off");
    } else if (strcasecmp(arg, "ON") == 0) {
        verify_on = 1; printf("VERIFY is on.\r\n");
    } else if (strcasecmp(arg, "OFF") == 0) {
        verify_on = 0; printf("VERIFY is off.\r\n");
    } else {
        printf("Must specify ON or OFF.\r\n");
    }
}

static void cmd_net_use(const char *rest) {
    if (!rest || !*rest) {
        printf("New connections will be remembered.\r\n\r\n\r\n"
               "There are no entries in the list.\r\n"); return;
    }
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc >= 2) {
        printf("The command completed successfully.\r\n");
    } else if (argc == 1 && strcmp(args[0], "/delete") == 0) {
        printf("There are no entries in the list.\r\n");
    } else {
        printf("The syntax of this command is:\r\nNET USE [devicename | *] [\\\\computername\\sharename[\\volume] [password | *]]\r\n");
    }
}

static void cmd_getmac(const char *rest) {
    (void)rest;
    printf("\r\nConnection Name         Network Adapter                 Physical Address    Transport Name\r\n"
           "======================= =============================== =================== ==========================================================\r\n"
           "Ethernet                Intel(R) Ethernet Connection    AA-BB-CC-DD-EE-FF   \\Device\\Tcpip_{00000000-0000-0000-0000-000000000000}\r\n"
           "Wi-Fi                   Intel(R) Wi-Fi 6 AX200          11-22-33-44-55-66   \\Device\\Tcpip_{00000000-0000-0000-0000-000000000001}\r\n");
}

static void cmd_gpupdate(const char *rest) {
    (void)rest;
    printf("Updating policy...\r\n\r\n"
           "Computer Policy update has completed successfully.\r\n"
           "User Policy update has completed successfully.\r\n");
}

static void cmd_openfiles(const char *rest) {
    (void)rest;
    printf("\r\nFiles Opened Remotely via local share points:\r\n\r\n"
           "INFO: No open file entries found.\r\n");
}

static void cmd_powercfg(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || (argc >= 1 && (strcasecmp(args[0], "/list") == 0 || strcasecmp(args[0], "-list") == 0 || strcasecmp(args[0], "/l") == 0))) {
        printf("Existing Power Schemes (* Active)\r\n-----------------------------------\r\nPower Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced) *\r\nPower Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)\r\nPower Scheme GUID: a1841308-3541-4fab-bc81-f71556f20b4a  (Power saver)\r\n");
    } else if (argc >= 1 && (strcasecmp(args[0], "/query") == 0 || strcasecmp(args[0], "-query") == 0 || strcasecmp(args[0], "/q") == 0)) {
        printf("Power Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced)\r\n"
               "  GUID Alias: SCHEME_BALANCED\r\n  Subgroup GUID: fea3413e-7e05-4911-9a71-700331f1c294  (Settings)\r\n"
               "    Power Setting GUID: 3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e  (Turn off hard disk after)\r\n"
               "      Minimum Possible Setting: 0x00000000\r\n      Maximum Possible Setting: 0xffffffff\r\n"
               "      Possible Settings increment: 0x00000001\r\n      Current AC Power Setting Index: 0x00000708\r\n"
               "      Current DC Power Setting Index: 0x00000258\r\n");
    } else if (argc >= 1 && (strcasecmp(args[0], "/energy") == 0 || strcasecmp(args[0], "-energy") == 0)) {
        printf("Tracing for 60 seconds...\r\nAnalyzing trace data...\r\n"
               "Analysis complete.\r\nEnergy efficiency problems were found.\r\n"
               "  1 Errors\r\n  2 Warnings\r\n  3 Informational\r\n"
               "See energy-report.html for more details.\r\n");
    } else if (argc >= 1 && (strcasecmp(args[0], "/batteryreport") == 0 || strcasecmp(args[0], "-batteryreport") == 0)) {
        printf("Battery life report saved to %s\\battery-report.html.\r\n", win_cwd);
    } else if (argc >= 1 && (strcasecmp(args[0], "/hibernate") == 0 || strcasecmp(args[0], "-hibernate") == 0)) {
        if (argc >= 2 && strcasecmp(args[1], "off") == 0)
            printf("Hibernation disabled.\r\n");
        else
            printf("Hibernation enabled.\r\n");
    } else {
        printf("POWERCFG /LIST\r\nPOWERCFG /QUERY [SCHEME_GUID] [SUB_GUID]\r\nPOWERCFG /CHANGE SETTING VALUE\r\nPOWERCFG /SETACTIVE SCHEME_GUID\r\nPOWERCFG /ENERGY\r\nPOWERCFG /BATTERYREPORT\r\nPOWERCFG /HIBERNATE [on|off]\r\n");
    }
}

static void cmd_rundll32(const char *rest) {
    if (!rest || !*rest) {
        printf("rundll32: no DLL specified.\r\nUsage: RUNDLL32 <dllname>,<entrypoint> [arguments]\r\n");
        return;
    }
    /* parse dll,entry */
    char buf[MAX_PATH];
    strncpy(buf, rest, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;
    char *comma = strchr(buf, ',');
    if (comma) {
        *comma = 0;
        printf("Executing %s entry point in %s...\r\n(Simulated — no actual DLL loaded.)\r\n", comma+1, buf);
    } else {
        printf("Executing %s...\r\n(Simulated — no actual DLL loaded.)\r\n", buf);
    }
}

static void cmd_tzutil(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || (argc >= 1 && strcasecmp(args[0], "/g") == 0)) {
        printf("UTC\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/l") == 0) {
        printf("(UTC-12:00) International Date Line West\r\n"
               "(UTC-08:00) Pacific Time (US & Canada)\r\n"
               "(UTC-07:00) Mountain Time (US & Canada)\r\n"
               "(UTC-06:00) Central Time (US & Canada)\r\n"
               "(UTC-05:00) Eastern Time (US & Canada)\r\n"
               "(UTC+00:00) UTC\r\n"
               "(UTC+01:00) W. Europe Standard Time\r\n"
               "(UTC+05:30) India Standard Time\r\n"
               "(UTC+08:00) China Standard Time\r\n"
               "(UTC+09:00) Tokyo Standard Time\r\n");
    } else if (argc >= 2 && strcasecmp(args[0], "/s") == 0) {
        printf("Time zone set to: %s\r\n", args[1]);
    } else {
        printf("TZUTIL /g          Display current time zone.\r\n"
               "TZUTIL /l          List all available time zones.\r\n"
               "TZUTIL /s TZ_ID    Set the time zone.\r\n");
    }
}

static void cmd_w32tm(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc >= 1 && strcasecmp(args[0], "/query") == 0) {
        printf("Leap Indicator: 0(no warning)\r\n"
               "Version Number: 3\r\n"
               "Mode: 3 (Client)\r\n"
               "Stratum: 3 (secondary reference - syncd by (S)NTP)\r\n"
               "Poll Interval: 10 (1024s)\r\n"
               "Precision: -23 (119.209ns per tick)\r\n"
               "Root Delay: 0.0312500s\r\n"
               "Root Dispersion: 7.7421875s\r\n"
               "ReferenceId: 0xC0A80101 (source IP:  192.168.1.1)\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/resync") == 0) {
        printf("Sending resync command to local computer\r\nThe command completed successfully.\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/stripchart") == 0) {
        printf("Tracking time.windows.com [40.119.6.228:123].\r\n"
               "Collecting 5 samples.\r\n"
               "The current time is 05/07/2026 12:00:00.\r\nd:+00.0000000s o:-00.0042167s  [ * ]\r\n"
               "d:+00.0000000s o:-00.0038291s  [ * ]\r\n");
    } else {
        printf("W32TM /query [/computer:target] [/source] [/configuration] [/peers] [/status]\r\n"
               "W32TM /resync [/computer:target] [/nowait] [/rediscover] [/soft]\r\n"
               "W32TM /stripchart /computer:target [/period:refresh] [/samples:count]\r\n"
               "W32TM /register\r\nW32TM /unregister\r\n");
    }
}

static void cmd_wbadmin(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("WBADMIN - Windows Backup Administration tool\r\n\r\n"
               "Commands:\r\n"
               "  START BACKUP      Creates a backup\r\n"
               "  STOP JOB          Stops the currently running backup or recovery\r\n"
               "  GET VERSIONS      Lists backups\r\n"
               "  GET ITEMS         Lists items in a backup\r\n"
               "  START RECOVERY    Runs a recovery\r\n"
               "  GET STATUS        Reports the status of the currently running operation\r\n"
               "  GET DISKS         Lists disks currently online\r\n"
               "  START SYSTEMSTATERECOVERY  Runs a system state recovery\r\n"
               "  START SYSTEMSTATEBACKUP    Backs up the system state\r\n"
               "  DELETE SYSTEMSTATEBACKUP   Deletes system state backups\r\n"
               "  ENABLE BACKUP     Enables or updates the Windows Server Backup schedule\r\n"
               "  DISABLE BACKUP    Disables the Windows Server Backup schedule\r\n"
               "  DELETE CATALOG    Deletes the backup catalog\r\n"
               "  RESTORE CATALOG   Recovers a backup catalog\r\n");
        return;
    }
    if (strcasecmp(args[0], "get") == 0 && argc >= 2 && strcasecmp(args[1], "versions") == 0) {
        printf("Backup time: 05/06/2026 03:00\r\nBackup target: 1394/USB Disk labeled Backup\r\nVersion identifier: 05/06/2026-03:00\r\nCan recover: Volume(s), File(s), Application(s), Bare Metal Recovery, System State\r\nSnapshot ID: {00000000-0000-0000-0000-000000000000}\r\n");
    } else if (strcasecmp(args[0], "get") == 0 && argc >= 2 && strcasecmp(args[1], "status") == 0) {
        printf("There is no currently running backup or recovery operation.\r\n");
    } else if (strcasecmp(args[0], "start") == 0 && argc >= 2 && strcasecmp(args[1], "backup") == 0) {
        printf("Starting backup operation...\r\nCreating a shadow copy of the volumes specified for backup...\r\nBackup of volume (C:) completed successfully.\r\nSummary of the backup operation:\r\n  The backup operation successfully completed.\r\n  The backup of volume C: completed successfully.\r\n");
    } else {
        printf("The command was not recognized. Run WBADMIN without arguments for usage.\r\n");
    }
}

static void cmd_wusa(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("Windows Update Standalone Installer\r\n\r\nUsage: wusa <update.msu> [/quiet] [/norestart] [/log:<logfile>]\r\n"
               "       wusa /uninstall /kb:<KBNumber> [/quiet] [/norestart]\r\n");
        return;
    }
    /* check for /uninstall */
    int uninstall = 0;
    char kb[32] = {0};
    char msu[MAX_PATH] = {0};
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(args[i], "/uninstall") == 0) uninstall = 1;
        else if (strncasecmp(args[i], "/kb:", 4) == 0) strncpy(kb, args[i]+4, 31);
        else if (args[i][0] != '/') strncpy(msu, args[i], MAX_PATH-1);
    }
    if (uninstall && *kb) {
        printf("Uninstalling update KB%s...\r\nThe update was successfully uninstalled.\r\n", kb);
    } else if (*msu) {
        printf("Installing update %s...\r\nThe update was successfully installed.\r\n", msu);
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}


static void cmd_secedit(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("SECEDIT /ANALYZE   /DB filename [/CFG filename] [/log filename] [/verbose] [/quiet]\r\n"
               "SECEDIT /CONFIGURE /DB filename [/CFG filename] [/overwrite] [/areas area1 area2...]\r\n"
               "SECEDIT /EXPORT    /CFG filename [/mergedpolicy] [/areas area1 area2...]\r\n"
               "SECEDIT /VALIDATE  filename\r\n"
               "SECEDIT /GENERATEROLLBACK /DB filename /CFG filename /rbk filename\r\n");
        return;
    }
    if (strcasecmp(args[0], "/analyze") == 0) {
        printf("Analyzing system security...\r\nTask is completed successfully.\r\n");
    } else if (strcasecmp(args[0], "/configure") == 0) {
        printf("Configuring system security...\r\nTask is completed successfully.\r\n");
    } else if (strcasecmp(args[0], "/export") == 0) {
        printf("Exporting security policy...\r\nTask is completed successfully.\r\n");
    } else if (strcasecmp(args[0], "/validate") == 0) {
        printf("Validating security template...\r\nTemplate is valid.\r\n");
    } else {
        printf("Invalid option.\r\n");
    }
}

static void cmd_logman(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("Microsoft (R) Logman\r\n\r\nUsage:\r\n"
               "  logman [create|query|start|stop|delete|update|import|export] [options]\r\n\r\n"
               "Commands:\r\n"
               "  create counter|trace|alert|cfg  Create a new data collector.\r\n"
               "  query [name|providers]           Query data collectors or providers.\r\n"
               "  start name                       Start an existing data collector.\r\n"
               "  stop  name                       Stop a running data collector.\r\n"
               "  delete name                      Delete a data collector.\r\n"
               "  update name                      Update a data collector.\r\n"
               "  import xmlfile                   Import data collectors from XML.\r\n"
               "  export name xmlfile              Export data collectors to XML.\r\n");
        return;
    }
    if (strcasecmp(args[0], "query") == 0) {
        if (argc >= 2 && strcasecmp(args[1], "providers") == 0) {
            printf("Provider                                 GUID\r\n"
                   "---------------------------------------- ------------------------------------\r\n"
                   "Microsoft-Windows-Kernel-Process         {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}\r\n"
                   "Microsoft-Windows-Kernel-File            {EDD08927-9CC4-4E65-B970-C2560FB5C289}\r\n"
                   "Microsoft-Windows-DNS-Client             {1C95126E-7EEA-49A9-A3FE-A378B03DDB4D}\r\n");
        } else {
            printf("Data Collector Set         Type              Status\r\n"
                   "-------------------------- ----------------- --------------------\r\n"
                   "System\\Active Directory     Performance       Stopped\r\n"
                   "System\\EventLog            Trace             Stopped\r\n"
                   "The command completed successfully.\r\n");
        }
    } else if (strcasecmp(args[0], "start") == 0 && argc >= 2) {
        printf("The command completed successfully.\r\n");
    } else if (strcasecmp(args[0], "stop") == 0 && argc >= 2) {
        printf("The command completed successfully.\r\n");
    } else if (strcasecmp(args[0], "delete") == 0 && argc >= 2) {
        printf("The command completed successfully.\r\n");
    } else {
        printf("The command completed successfully.\r\n");
    }
}

static void cmd_reagentc(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || (argc >= 1 && strcasecmp(args[0], "/info") == 0)) {
        printf("Windows Recovery Environment (Windows RE) and system reset tool\r\n\r\n"
               "ReAgent.xml         C:\\Windows\\System32\\Recovery\r\n"
               "REAGENTC.EXE version: 10.0.19041.1\r\n\r\n"
               "Windows RE status:         Enabled\r\n"
               "Windows RE location:       \\\\?\\GLOBALROOT\\device\\harddisk0\\partition4\\Recovery\\WindowsRE\r\n"
               "Boot Configuration Data (BCD) identifier: 00000000-0000-0000-0000-000000000000\r\n"
               "Recovery image location:\r\n"
               "Recovery image index:      0\r\n"
               "Custom image location:\r\n"
               "Custom image index:        0\r\n\r\n"
               "REAGENTC.EXE operation successful.\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/enable") == 0) {
        printf("REAGENTC.EXE operation successful.\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/disable") == 0) {
        printf("REAGENTC.EXE operation successful.\r\n");
    } else if (argc >= 1 && strcasecmp(args[0], "/boottore") == 0) {
        printf("Operation successful: Boot to Windows RE is configured.\r\nREAGENTC.EXE operation successful.\r\n");
    } else {
        printf("REAGENTC /info\r\nREAGENTC /enable\r\nREAGENTC /disable\r\nREAGENTC /boottore\r\nREAGENTC /setreimage /path <path>\r\n");
    }
}

static void cmd_dxdiag(const char *rest) {
    (void)rest;
    printf("Starting DirectX Diagnostic Tool...\r\n\r\n"
           "------------------\r\nSystem Information\r\n------------------\r\n"
           "Current Date/Time:  05/07/2026, 12:00:00\r\n"
           "Computer Name:      DESKTOP-WINTERM\r\n"
           "Operating System:   Windows 10 Pro 64-bit (10.0, Build 19045)\r\n"
           "System Manufacturer: WinTerm Simulated Hardware\r\n"
           "System Model:       WinTerm v1.0\r\n"
           "BIOS:               BIOS Date: 01/01/2024, Version: 1.0\r\n"
           "Processor:          AMD64 Family 23 Model 113 (12 CPUs), ~3.6GHz\r\n"
           "Memory:             16384MB RAM\r\n"
           "Page File:          18432MB used, 6144MB available\r\n"
           "Windows Dir:        C:\\Windows\r\n"
           "DirectX Version:    DirectX 12\r\n\r\n"
           "-------------------\r\nDisplay Information\r\n-------------------\r\n"
           "Card Name:          Simulated GPU\r\n"
           "Manufacturer:       WinTerm\r\n"
           "Chip Type:          WinTerm Virtual GPU\r\n"
           "DAC Type:           Integrated RAMDAC\r\n"
           "Device Type:        Full Device (POST)\r\n"
           "Display Memory:     4096 MB\r\n"
           "Dedicated Memory:   2048 MB\r\n"
           "Shared Memory:      2048 MB\r\n"
           "Current Mode:       1920 x 1080 (32 bit) (60Hz)\r\n"
           "Monitor Name:       Generic PnP Monitor\r\n"
           "Driver Name:        winterm_disp.dll\r\n"
           "Driver Version:     10.0.19041.0000\r\n"
           "DDI Version:        12\r\n"
           "Feature Levels:     12_1,12_0,11_1,11_0,10_1,10_0\r\n\r\n"
           "No problems found.\r\n");
}

static void cmd_recover(const char *rest) {
    if (!rest || !*rest) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, rest);
    FILE *f = fopen(real, "rb");
    if (!f) { printf("The system cannot find the file specified.\r\n"); return; }
    fclose(f);
    printf("%s\r\nRecovered 1 file(s) from %s.\r\n", rest, win_cwd);
}

static void cmd_waitfor(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("The syntax of the command is incorrect.\r\nUsage: WAITFOR [/S system] [/T timeout] signal\r\n       WAITFOR /SI signal\r\n");
        return;
    }
    int timeout = 0;
    const char *signal = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(args[i], "/T") == 0 && i + 1 < argc) {
            timeout = atoi(args[++i]);
        } else if (strcasecmp(args[i], "/SI") == 0 && i + 1 < argc) {
            printf("Signal %s sent successfully.\r\n", args[++i]);
            return;
        } else if (args[i][0] != '/') {
            signal = args[i];
        }
    }
    if (signal) {
        if (timeout > 0)
            printf("Waiting for signal '%s' (timeout: %ds) ...\r\nWAITFOR: No sources currently waiting for signal %s, broadcasting anyway.\r\n", signal, timeout, signal);
        else
            printf("Waiting for signal '%s' ...\r\nWAITFOR: No sources currently waiting for signal %s.\r\n", signal, signal);
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_wevtutil(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("Windows Events Command Line Utility.\r\n\r\n"
               "Commands:\r\nel | enum-logs          List log names.\r\n"
               "gl | get-log            Get log configuration information.\r\n"
               "sl | set-log            Modify configuration of a log.\r\n"
               "ep | enum-publishers    List event publishers.\r\n"
               "qe | query-events       Query events from a log or log file.\r\n"
               "cl | clear-log          Clear a log.\r\n");
        return;
    }
    if (strcasecmp(args[0], "el") == 0 || strcasecmp(args[0], "enum-logs") == 0) {
        printf("Application\r\nHardwareEvents\r\nKey Management Service\r\nSecurity\r\nSystem\r\nWindows PowerShell\r\n");
    } else if ((strcasecmp(args[0], "cl") == 0 || strcasecmp(args[0], "clear-log") == 0) && argc >= 2) {
        printf("Log %s cleared successfully.\r\n", args[1]);
    } else if ((strcasecmp(args[0], "gl") == 0 || strcasecmp(args[0], "get-log") == 0) && argc >= 2) {
        printf("name: %s\r\nenabled: true\r\ntype: Admin\r\nisolation: Application\r\nlogging:\r\n"
               "  logFileName: %%SystemRoot%%\\System32\\Winevt\\Logs\\%s.evtx\r\n"
               "  retention: false\r\n  autoBackup: false\r\n  maxSize: 1052672\r\n",
               args[1], args[1]);
    } else {
        printf("Command not recognized or not yet simulated.\r\n");
    }
}

/* ── Missing commands ────────────────────────────────────────────────────── */

static void cmd_append(const char *arg) {
    if (!arg || !*arg) { printf("No Append; APPEND=\r\n"); return; }
    if (strcasecmp(arg, ";") == 0) { printf("Append disabled.\r\n"); return; }
    printf("APPEND=%s\r\n", arg);
}

static void cmd_cacls(const char *arg) {
    if (!arg || !*arg) { printf("The syntax of the command is incorrect.\r\n"); return; }
    char real[MAX_PATH];
    snprintf(real, sizeof(real), "%s/%s", real_cwd, arg);
    struct stat st;
    if (stat(real, &st) != 0) { printf("The system cannot find the file specified.\r\n"); return; }
    printf("C:\\%s BUILTIN\\Administrators:(OI)(CI)F\r\n"
           "         NT AUTHORITY\\SYSTEM:(OI)(CI)F\r\n"
           "         BUILTIN\\Users:(OI)(CI)R\r\n"
           "         NT AUTHORITY\\Authenticated Users:(OI)(CI)(IO)M\r\n"
           "         NT AUTHORITY\\Authenticated Users:(AD)\r\n", arg);
}

static void cmd_chcp(const char *arg) {
    if (!arg || !*arg) {
        printf("Active code page: 437\r\n"); return;
    }
    int cp = atoi(arg);
    if (cp == 437 || cp == 850 || cp == 1252 || cp == 65001) {
        printf("Active code page: %d\r\n", cp);
    } else {
        printf("Invalid code page.\r\n");
    }
}

static void cmd_cmdkey(const char *arg) {
    if (!arg || !*arg || strstr(arg, "/?")) {
        printf("Creates, displays, and deletes stored user names and passwords.\r\n\r\n"
               "CMDKEY [/add:target /user:user /pass:pass] [/delete:target] [/list[:target]]\r\n"); return;
    }
    if (strstr(arg, "/list")) {
        printf("Currently stored credentials:\r\n\r\n"
               "    Target: DESKTOP-WINTERM\r\n"
               "    Type: Generic\r\n"
               "    User: Administrator\r\n\r\n"
               "There is 1 stored credential.\r\n");
    } else if (strstr(arg, "/add")) {
        printf("CMDKEY: Credential added successfully.\r\n");
    } else if (strstr(arg, "/delete")) {
        printf("CMDKEY: Credential deleted successfully.\r\n");
    } else {
        printf("CMDKEY: Invalid argument.\r\n");
    }
}

static void cmd_cscript(const char *arg) {
    if (!arg || !*arg) {
        printf("Microsoft (R) Windows Script Host Version 5.812\r\n"
               "Copyright (C) Microsoft Corporation. All rights reserved.\r\n\r\n"
               "Usage: CScript scriptname.extension [option...] [arguments...]\r\n"); return;
    }
    printf("Microsoft (R) Windows Script Host Version 5.812\r\n"
           "Copyright (C) Microsoft Corporation. All rights reserved.\r\n\r\n"
           "CScript: '%s' is not recognized as a script file.\r\n", arg);
}

static void cmd_wscript(const char *arg) {
    if (!arg || !*arg) {
        printf("Windows Script Host\r\nUsage: WScript scriptname.extension [option...] [arguments...]\r\n"); return;
    }
    printf("WScript: '%s' cannot be run in this terminal emulator.\r\n", arg);
}

static void cmd_diskcomp(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    printf("Comparing floppy disks...\r\n\r\n"
           "Compare OK\r\n\r\nCompare another diskette (Y/N)? ");
    fflush(stdout);
    char c[4]; if (fgets(c, sizeof(c), stdin)) {}
    printf("\r\n");
}

static void cmd_diskcopy(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return; }
    printf("Insert SOURCE diskette in drive %s:\r\n"
           "Press any key to continue . . .\r\n", args[0]);
    fflush(stdout); getchar();
    printf("Copying 80 tracks, 18 sectors per track, 2 side(s)\r\n\r\n"
           "Copy another diskette (Y/N)? ");
    fflush(stdout);
    char c[4]; if (fgets(c, sizeof(c), stdin)) {}
    printf("\r\n");
}

static void cmd_finger(const char *arg) {
    if (!arg || !*arg) { printf("Usage: finger [user]@host\r\n"); return; }
    printf("This system does not support the finger protocol.\r\n");
}

static void cmd_ftp(const char *arg) {
    (void)arg;
    printf("ftp> ");
    fflush(stdout);
    char line[256];
    if (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line,"\r\n")] = 0;
        if (strcasecmp(line,"quit")==0||strcasecmp(line,"bye")==0||strcasecmp(line,"exit")==0)
            printf("\r\n");
        else
            printf("?Invalid command\r\n");
    }
}

static void cmd_mem(const char *arg) {
    (void)arg;
    printf("\r\n"
           "Memory Type        Total       Used       Free\r\n"
           "----------------   ----------  ---------  ----------\r\n"
           "Conventional           640K       16K         624K\r\n"
           "Upper                    0K        0K           0K\r\n"
           "Reserved                 0K        0K           0K\r\n"
           "Extended (XMS)    16,776,192K  8,388,608K  8,387,584K\r\n"
           "----------------   ----------  ---------  ----------\r\n"
           "Total memory      16,776,832K  8,388,624K  8,388,208K\r\n\r\n"
           "Total under 1 MB       640K       16K         624K\r\n\r\n"
           "Largest executable program size         624K (638,976 bytes)\r\n"
           "Largest free upper memory block           0K (0 bytes)\r\n"
           "MS-DOS is resident in the high memory area.\r\n");
}

static void cmd_mode(const char *arg) {
    if (!arg || !*arg) {
        printf("Status for device CON:\r\n"
               "-----------------------\r\n"
               "    Lines:          25\r\n"
               "    Columns:        80\r\n"
               "    Keyboard rate:  31\r\n"
               "    Keyboard delay: 1\r\n"
               "    Code page:      437\r\n"); return;
    }
    if (strncasecmp(arg, "CON", 3) == 0 || strncasecmp(arg, "con", 3) == 0) {
        printf("Mode applied.\r\n");
    } else if (strncasecmp(arg, "COM", 3) == 0) {
        printf("Serial port mode set.\r\n");
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_mstsc(const char *arg) {
    (void)arg;
    printf("Remote Desktop Connection cannot be launched from this terminal emulator.\r\n");
}

static void cmd_nltest(const char *arg) {
    if (!arg || !*arg || strstr(arg,"/?")) {
        printf("Usage: nltest [/OPTIONS]\r\n\r\n"
               "  /SC_QUERY:<domain>     Query secure channel.\r\n"
               "  /DCLIST:<domain>       Get list of DCs.\r\n"
               "  /DSGETDC:<domain>      Get DC name.\r\n"); return;
    }
    if (strstr(arg, "/DSGETDC") || strstr(arg, "/dsgetdc")) {
        printf("DC: \\\\DESKTOP-WINTERM\r\nAddress: \\\\192.168.1.100\r\nDom Guid: {00000000-0000-0000-0000-000000000000}\r\n"
               "Dom Name: WORKGROUP\r\nForest Name: WORKGROUP\r\nFlags: 0xE00031FD  PDC DS KDC TIMESERV WRITABLE DNS_DC DNS_DOMAIN DNS_FOREST CLOSE_SITE FULL_SECRET\r\n"
               "The command completed successfully.\r\n");
    } else if (strstr(arg, "/SC_QUERY") || strstr(arg, "/sc_query")) {
        printf("Flags: 0\r\nConnection Status = 0 0x0 NERR_Success\r\nThe command completed successfully.\r\n");
    } else {
        printf("The command completed successfully.\r\n");
    }
}

static void cmd_qprocess(const char *arg) {
    (void)arg;
    printf(" USERNAME              SESSIONNAME        ID  PID  IMAGE\r\n"
           " administrator         console              1 5168  cmd.exe\r\n"
           " administrator         console              1 5200  winterm.exe\r\n");
}

static void cmd_query(const char *rest) {
    char sub[32] = {0};
    const char *p = rest ? rest : "";
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;

    if (strcmp(sub, "PROCESS") == 0 || strcmp(sub, "PROC") == 0) {
        cmd_qprocess(p);
    } else if (strcmp(sub, "SESSION") == 0) {
        printf(" SESSIONNAME    USERNAME         ID  STATE   TYPE        DEVICE\r\n"
               " console        administrator     1  Active  wdcon\r\n");
    } else if (strcmp(sub, "USER") == 0) {
        printf(" USERNAME              SESSIONNAME        ID  STATE   IDLE TIME  LOGON TIME\r\n"
               " administrator         console              1  Active      none  5/7/2026 12:00 AM\r\n");
    } else if (strcmp(sub, "TERMSERVER") == 0) {
        printf("Known Remote Desktop Session Host server in current domain:\r\n DESKTOP-WINTERM\r\n");
    } else {
        printf("The syntax of the command is incorrect.\r\n"
               "Usage: QUERY PROCESS|SESSION|USER|TERMSERVER\r\n");
    }
}


static void cmd_rpcping(const char *arg) {
    if (!arg || !*arg) { printf("Usage: rpcping [-s Server] [-e Endpoint] [-a AuthnLevel] [-u AuthnSvc]\r\n"); return; }
    const char *server = strstr(arg, "-s ");
    if (server) {
        server += 3;
        char host[64] = {0}; int j = 0;
        while (server[j] && !isspace((unsigned char)server[j]) && j < 63) { host[j] = server[j]; j++; }
        printf("RPCPing to server %s succeeded.\r\n", host);
    } else {
        printf("RPCPing requires a server (-s).\r\n");
    }
}

static void cmd_setver(const char *arg) {
    (void)arg;
    printf("SETVER is not supported on 64-bit Windows.\r\n");
}

static void cmd_share(const char *arg) {
    (void)arg;
    printf("There are no entries in the list.\r\n");
}

static void cmd_sxstrace(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: sxstrace.exe [{Trace -logfile:<filename> [-nostop]}|{Parse -logfile:<filename> -outfile:<filename>}]\r\n"); return;
    }
    printf("The operation completed successfully.\r\n");
}

static void cmd_typeperf(const char *arg) {
    if (!arg || !*arg) { printf("Usage: typeperf <counter [counter ...]> [options]\r\n"); return; }
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%m/%d/%Y %H:%M:%S.000", tm);
    printf("\"(PDH-CSV 4.0)\",\"%s\"\r\n", arg);
    printf("\"%s\",\"0.000000\"\r\n", ts);
    printf("Exiting, please wait...\r\nThe command completed successfully.\r\n");
}

static void cmd_unlodctr(const char *arg) {
    if (!arg || !*arg) { printf("Usage: unlodctr <driver>\r\n"); return; }
    printf("Removing counter names and explanations for %s...\r\n"
           "The operation completed successfully.\r\n", arg);
}

static void cmd_vssadmin(const char *rest) {
    char sub[32] = {0};
    const char *p = rest ? rest : "";
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);

    if (!*sub) {
        printf("vssadmin 1.1 - Volume Shadow Copy Service administrative command-line tool\r\n"
               "(C) Copyright 2001-2013 Microsoft Corp.\r\n\r\n"
               "---- Commands Supported ----\r\n\r\n"
               "Delete Shadows        - Delete volume shadow copies\r\n"
               "List Providers        - List registered volume shadow copy providers\r\n"
               "List Shadows          - List existing volume shadow copies\r\n"
               "List ShadowStorage    - List volume shadow copy storage associations\r\n"
               "List Volumes          - List volumes eligible for shadow copies\r\n"
               "List Writers          - List subscribed volume shadow copy writers\r\n"
               "Resize ShadowStorage  - Resize a volume shadow copy storage association\r\n"); return;
    }
    if (strcmp(sub, "LIST") == 0) {
        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p == ' ') p++;
        char what[32] = {0}; int j = 0;
        while (*p && !isspace((unsigned char)*p) && j < 31) what[j++] = toupper((unsigned char)*p++);
        if (strcmp(what, "SHADOWS") == 0) {
            printf("vssadmin 1.1 - Volume Shadow Copy Service administrative command-line tool\r\n"
                   "(C) Copyright 2001-2013 Microsoft Corp.\r\n\r\n"
                   "No items found that satisfy the query.\r\n");
        } else if (strcmp(what, "PROVIDERS") == 0) {
            printf("vssadmin 1.1 - Volume Shadow Copy Service administrative command-line tool\r\n"
                   "(C) Copyright 2001-2013 Microsoft Corp.\r\n\r\n"
                   "Provider name: 'Microsoft Software Shadow Copy provider 1.0'\r\n"
                   "   Provider type: System\r\n"
                   "   Provider Id: {b5946137-7b9f-4925-af80-51abd60b20d5}\r\n"
                   "   Version: 1.0.0.7\r\n\r\n");
        } else if (strcmp(what, "VOLUMES") == 0) {
            printf("vssadmin 1.1 - Volume Shadow Copy Service administrative command-line tool\r\n"
                   "(C) Copyright 2001-2013 Microsoft Corp.\r\n\r\n"
                   "Volume path: C:\\\r\n"
                   "    Volume name: \\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\\r\n\r\n");
        } else {
            printf("The operation completed successfully.\r\n");
        }
    } else if (strcmp(sub, "DELETE") == 0) {
        printf("Do you really want to delete 0 shadow copies (Y/N): [N] ");
        fflush(stdout);
        char c[4]; if (fgets(c, sizeof(c), stdin)) {}
        printf("\r\nSuccessfully deleted 0 shadow copies.\r\n");
    } else if (strcmp(sub, "RESIZE") == 0) {
        printf("Successfully resized the shadow copy storage association.\r\n");
    } else {
        printf("The operation completed successfully.\r\n");
    }
}

static void cmd_wecutil(const char *rest) {
    char sub[32] = {0};
    const char *p = rest ? rest : "";
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    if (!*sub) {
        printf("Windows Event Collector Utility\r\n\r\n"
               "Enables you to create and manage subscriptions to events forwarded from remote\r\n"
               "event sources that support WS-Management protocol.\r\n\r\n"
               "Usage:\r\n\r\n"
               "wecutil COMMAND [ARGUMENT [ARGUMENT] ...] [/OPTION:VALUE [/OPTION:VALUE] ...]\r\n\r\n"
               "Commands:\r\n"
               "es (enum-subscription)    List existing subscriptions.\r\n"
               "gs (get-subscription)     Get subscription configuration.\r\n"
               "gr (get-subscriptionruntimestatus) Get subscription runtime status.\r\n"
               "ss (set-subscription)     Set subscription configuration.\r\n"
               "cs (create-subscription)  Create new subscription.\r\n"
               "ds (delete-subscription)  Delete subscription.\r\n"
               "rs (retry-subscription)   Retry subscription.\r\n"
               "qc (quick-config)         Configure Event Collector service.\r\n"); return;
    }
    if (strcmp(sub,"ES")==0 || strcmp(sub,"ENUM-SUBSCRIPTION")==0) {
        printf("(No subscriptions found)\r\n");
    } else if (strcmp(sub,"QC")==0 || strcmp(sub,"QUICK-CONFIG")==0) {
        printf("The operation completed successfully.\r\n");
    } else {
        printf("The operation completed successfully.\r\n");
    }
}

static void cmd_winrm(const char *rest) {
    (void)rest;
    printf("Windows Remote Management Command Line Tool\r\n\r\n"
           "Windows Remote Management (WinRM) is the Microsoft implementation of\r\n"
           "the WS-Management protocol which provides a common way for systems to\r\n"
           "access and exchange management information.\r\n\r\n"
           "Usage:\r\n    winrm OPERATION RESOURCE_URI [-SWITCH:VALUE [-SWITCH:VALUE] ...]\r\n"
           "                  [@{KEY=\"VALUE\"[;KEY=\"VALUE\"]}]\r\n\r\n"
           "For help on a specific operation:\r\n"
           "    winrm help OPERATION\r\n\r\n"
           "Operations: configsddl create delete enumerate get help invoke put set\r\n\r\n"
           "Shortcuts:\r\n"
           "    winrm qc\r\n"
           "    winrm quickconfig\r\n"
           "    winrm id\r\n"
           "    winrm invoke\r\n"
           "    winrm delete\r\n"
           "    winrm create\r\n"
           "    winrm set\r\n"
           "    winrm get\r\n"
           "    winrm enumerate\r\n");
}

static void cmd_winrs(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: winrs [-r:endpoint] [-u:username] [-p:password] [command]\r\n"); return;
    }
    printf("Winrs: Remote shell execution is not available in this emulator.\r\n");
}

static void cmd_wpeutil(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: wpeutil command [parameters]\r\n"
               "Commands: InitializeNetwork, Reboot, Shutdown, UpdateBootInfo,\r\n"
               "          DisableFirewall, EnableFirewall, SaveProfile, RestoreProfile\r\n"); return;
    }
    char sub[32] = {0}; int i = 0;
    while (arg[i] && !isspace((unsigned char)arg[i]) && i < 31) { sub[i] = toupper((unsigned char)arg[i]); i++; }
    if (strcmp(sub,"REBOOT")==0) { printf("Rebooting...\r\n"); }
    else if (strcmp(sub,"SHUTDOWN")==0) { printf("Shutting down...\r\n"); exit(0); }
    else if (strcmp(sub,"INITIALIZENETWORK")==0) { printf("Network initialized.\r\n"); }
    else { printf("The operation completed successfully.\r\n"); }
}

static void cmd_lodctr(const char *arg) {
    if (!arg || !*arg) { printf("Usage: lodctr <filename>\r\n"); return; }
    printf("Info: Successfully installed performance counters in %s\r\n", arg);
}

static void cmd_net1(const char *rest) {
    /* NET1 is legacy alias for NET - forward declared below */
    void cmd_net(const char *);
    cmd_net(rest);
}

static void cmd_perfmon(const char *arg) {
    (void)arg;
    printf("Performance Monitor is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_iexpress(const char *arg) {
    (void)arg;
    printf("IExpress Wizard is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_control(const char *arg) {
    (void)arg;
    printf("Control Panel is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_mmc(const char *arg) {
    (void)arg;
    printf("Microsoft Management Console is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_mspaint(const char *arg) {
    (void)arg;
    printf("Paint is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_notepad(const char *arg) {
    if (!arg || !*arg) { printf("Notepad is a GUI application and cannot run in this terminal.\r\n"); return; }
    /* fallback: type the file */
    cmd_type(arg);
}

static void cmd_calc(const char *arg) {
    (void)arg;
    printf("Calculator is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_taskmgr(const char *arg) {
    (void)arg;
    printf("Task Manager is a GUI application. Use TASKLIST and TASKKILL instead.\r\n");
}

static void cmd_explorer(const char *arg) {
    (void)arg;
    printf("Windows Explorer is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_charmap(const char *arg) {
    (void)arg;
    printf("Character Map is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_cleanmgr(const char *arg) {
    (void)arg;
    printf("Disk Cleanup is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_eventvwr(const char *arg) {
    (void)arg;
    printf("Event Viewer is a GUI application. Use WEVTUTIL instead.\r\n");
}

static void cmd_msinfo32(const char *arg) {
    (void)arg;
    /* text fallback */
    cmd_systeminfo();
}

static void cmd_services(const char *arg) {
    (void)arg;
    printf("Services console is a GUI application. Use SC instead.\r\n");
}

static void cmd_devmgmt(const char *arg) {
    (void)arg;
    printf("Device Manager is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_compmgmt(const char *arg) {
    (void)arg;
    printf("Computer Management is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_secpol(const char *arg) {
    (void)arg;
    printf("Local Security Policy is a GUI application. Use SECEDIT instead.\r\n");
}

static void cmd_gpedit(const char *arg) {
    (void)arg;
    printf("Group Policy Editor is a GUI application. Use GPRESULT or GPUPDATE instead.\r\n");
}

static void cmd_lusrmgr(const char *arg) {
    (void)arg;
    printf("Local Users and Groups is a GUI application. Use NET USER instead.\r\n");
}

static void cmd_odbcad32(const char *arg) {
    (void)arg;
    printf("ODBC Data Source Administrator is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_printui(const char *arg) {
    (void)arg;
    printf("Printer configuration is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_osk(const char *arg) {
    (void)arg;
    printf("On-Screen Keyboard is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_magnify(const char *arg) {
    (void)arg;
    printf("Magnifier is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_narrator(const char *arg) {
    (void)arg;
    printf("Narrator is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_snippet(const char *arg) {
    (void)arg;
    printf("Snipping Tool is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_write(const char *arg) {
    if (arg && *arg) cmd_type(arg);
    else printf("WordPad is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_xcopy_ext(const char *args_str) {
    /* already have cmd_xcopy, this is a no-op alias guard */
    extern void cmd_xcopy(const char *);
    cmd_xcopy(args_str);
}

static void cmd_quser(const char *arg) {
    (void)arg;
    printf(" USERNAME              SESSIONNAME        ID  STATE   IDLE TIME  LOGON TIME\r\n"
           " administrator         console              1  Active      none  5/7/2026 12:00 AM\r\n");
}

static void cmd_qwinsta(const char *arg) {
    (void)arg;
    printf(" SESSIONNAME    USERNAME         ID  STATE   TYPE        DEVICE\r\n"
           " console        administrator     1  Active  wdcon\r\n");
}

static void cmd_rwinsta(const char *arg) {
    if (!arg || !*arg) { printf("Usage: RWINSTA sessionid|sessionname\r\n"); return; }
    printf("The session was reset successfully.\r\n");
}

static void cmd_logoff(const char *arg) {
    (void)arg;
    printf("Logging off session...\r\n");
}

static void cmd_msg(const char *arg) {
    if (!arg || !*arg) { printf("Usage: MSG {username | sessionname | sessionid | @filename | *} [/time:seconds] [message]\r\n"); return; }
    /* parse: MSG target message */
    const char *p = arg;
    char target[64] = {0}; int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 63) target[i++] = *p++;
    while (*p == ' ') p++;
    printf("Msg was sent to %s.\r\n", target);
}

static void cmd_shadow(const char *arg) {
    if (!arg || !*arg) { printf("Usage: SHADOW {sessionname | sessionid} [/server:servername] [/v]\r\n"); return; }
    printf("Your session may be monitored.\r\n"
           "Do you accept the responsibility? [Yes/No]: ");
    fflush(stdout);
    char c[8]; if (fgets(c, sizeof(c), stdin)) {}
    printf("\r\nShadowing session %s.\r\n", arg);
}

static void cmd_tsdiscon(const char *arg) {
    (void)arg;
    printf("Session disconnected.\r\n");
}

static void cmd_tscon(const char *arg) {
    if (!arg || !*arg) { printf("Usage: TSCON sessionid [/dest:sessionname] [/password:pw] [/v]\r\n"); return; }
    printf("Session %s connected.\r\n", arg);
}

static void cmd_change(const char *rest) {
    char sub[32] = {0}; int i = 0;
    const char *p = rest ? rest : "";
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;
    if (strcmp(sub,"USER")==0) {
        printf("Install mode enabled.\r\n");
    } else if (strcmp(sub,"PORT")==0) {
        printf("COM port mapping:\r\nCOM1 = \\Device\\Serial0\r\n");
    } else if (strcmp(sub,"LOGON")==0) {
        printf("Logon scripts are enabled.\r\n");
    } else {
        printf("The syntax of the command is incorrect.\r\n");
    }
}

static void cmd_tskill(const char *arg) {
    if (!arg || !*arg) { printf("Usage: TSKILL processid | processname [/server:servername] [/id:sessionid | /a] [/v]\r\n"); return; }
    printf("PID %s killed.\r\n", arg);
}

static void cmd_flattemp(const char *arg) {
    if (!arg || !*arg) { printf("Usage: FLATTEMP {/query | /enable | /disable}\r\n"); return; }
    if (strstr(arg,"/query")) printf("Temporary folders per session: disabled\r\n");
    else printf("The operation completed successfully.\r\n");
}

static void cmd_register_srvexe(void) {
    printf("Windows Terminal Server configuration is not available in this emulator.\r\n");
}

static void cmd_dfsvc(void) {
    printf("ClickOnce application deployment is a GUI function and cannot run in this terminal.\r\n");
}

static void cmd_pkgmgr(const char *arg) {
    (void)arg;
    printf("Package Manager (pkgmgr.exe) is deprecated. Use DISM instead.\r\n");
}

static void cmd_pcalua(const char *arg) {
    (void)arg;
    printf("Program Compatibility Assistant is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_cttune(void) {
    printf("ClearType Tuner is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_dpiscaling(void) {
    printf("DPI Scaling configuration is a GUI function and cannot run in this terminal.\r\n");
}

static void cmd_hdwwiz(void) {
    printf("Add Hardware Wizard is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_credwiz(void) {
    printf("Credential Backup and Restore Wizard is a GUI application.\r\n");
}

static void cmd_eudcedit(void) {
    printf("Private Character Editor is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_fsmgmt(void) {
    printf("Shared Folders MMC snap-in is a GUI application. Use NET SHARE instead.\r\n");
}

static void cmd_sysdm(void) {
    printf("System Properties is a GUI application. Use SYSTEMINFO or WMIC instead.\r\n");
}

static void cmd_netplwiz(void) {
    printf("User Accounts control panel is a GUI application. Use NET USER instead.\r\n");
}

static void cmd_timedate(void) {
    cmd_date_cmd();
    cmd_time_cmd();
}

static void cmd_colorcpl(void) {
    printf("Color Management is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_mblctr(void) {
    printf("Windows Mobility Center is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_bitsadmin(const char *rest) {
    char sub[32] = {0}; int i = 0;
    const char *p = rest ? rest : "";
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    if (!*sub || strcmp(sub,"/?")==0 || strcmp(sub,"HELP")==0) {
        printf("BITSADMIN version 3.0 [ 7.5.7601 ]\r\n"
               "BITS administration utility.\r\n"
               "(C) Copyright 2000-2006 Microsoft Corp.\r\n\r\n"
               "usage: bitsadmin /command\r\n\r\n"
               "  /List [/allusers] [/verbose]\r\n"
               "  /Monitor [/allusers] [/refresh seconds]\r\n"
               "  /Reset [/allusers]\r\n"
               "  /Transfer name [type] [/priority priority] [/ACLflags flags]\r\n"
               "           remote local\r\n"); return;
    }
    if (strcmp(sub,"/LIST")==0) {
        printf("0 jobs.\r\n");
    } else if (strcmp(sub,"/RESET")==0) {
        printf("0 out of 0 jobs canceled.\r\n");
    } else if (strcmp(sub,"/TRANSFER")==0) {
        printf("BITSADMIN: Transfer complete.\r\n");
    } else {
        printf("The command completed successfully.\r\n");
    }
}

static void cmd_makecab(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 1) { printf("Usage: MAKECAB [/V[n]] [/D var=value ...] [/L dir] source [destination]\r\n"); return; }
    printf("Cabinet Maker - Lossless Data Compression Tool\r\n\r\n"
           "1,050,926 bytes in 1 files\r\nTotal files:              1\r\nBytes before:     1,050,926\r\nBytes after:        412,680\r\n"
           "After/Before:            39.27%% compression\r\nTime:                  0.01 seconds ( 0.00 Kb/s)\r\nDone.\r\n");
}

static void cmd_expand_cab(const char *args_str) {
    /* cmd_expand already exists; this is the CAB-specific form — forward */
    extern void cmd_expand(const char *);
    cmd_expand(args_str);
}

static void cmd_extrac32(const char *args_str) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(args_str, args, &argc);
    if (argc < 1) { printf("Usage: EXTRAC32 cabinet [/Y] [/A] [/D] [/E] [/L dir] [files...]\r\n"); return; }
    printf("Microsoft (R) Cabinet Extraction Tool\r\nCopyright (c) Microsoft Corporation. All rights reserved.\r\n\r\n"
           "Extracting cabinet: %s\r\nExtracted 0 file(s).\r\n", args[0]);
}

static void cmd_wmic_alias(const char *rest) { cmd_wmic(rest); }

static void cmd_psr(const char *arg) {
    (void)arg;
    printf("Problem Steps Recorder is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_xwizard(const char *arg) {
    (void)arg;
    printf("Extensible Wizard is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_rekeywiz(const char *arg) {
    (void)arg;
    printf("Encrypting File System Wizard is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_sdclt(const char *arg) {
    (void)arg;
    printf("Backup and Restore is a GUI application. Use WBADMIN instead.\r\n");
}

static void cmd_recdisc(const char *arg) {
    (void)arg;
    printf("Recovery Drive Creator is a GUI application and cannot run in this terminal.\r\n");
}

static void cmd_checknetisolation(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: CheckNetIsolation Command [Options]\r\nCommands: LoopbackExempt -a|-d|-s, Debug -p:<pid>\r\n"); return;
    }
    printf("OK.\r\n");
}

static void cmd_icacls_ext(const char *args_str) {
    extern void cmd_icacls(const char *);
    cmd_icacls(args_str);
}

/* ── IF ──────────────────────────────────────────────────────────────────── */

static void cmd_if(const char *rest) {
    int negate = 0;
    const char *p = rest;
    char tmp[MAX_INPUT];
    strncpy(tmp, p, sizeof(tmp));

    if (strncasecmp(tmp, "NOT ", 4) == 0) { negate = 1; p = tmp + 4; }
    else p = tmp;

    char lhs[256] = {0}, rhs[256] = {0};
    const char *eq = strstr(p, "==");
    if (!eq) { printf("Incorrect IF syntax.\r\n"); return; }
    int llen = eq - p;
    strncpy(lhs, p, llen < 255 ? llen : 255);
    strncpy(rhs, eq + 2, 255);
    for (int i = 0; i < 2; i++) {
        char *s = (i == 0) ? lhs : rhs;
        int sl = strlen(s);
        if (sl >= 2 && s[0] == '"' && s[sl-1] == '"') { s[sl-1] = 0; memmove(s, s+1, sl-1); }
    }
    char *cmd_part = rhs;
    while (*cmd_part && *cmd_part != ' ') cmd_part++;
    char subcmd[MAX_INPUT] = {0};
    if (*cmd_part) { *cmd_part = 0; strncpy(subcmd, cmd_part + 1, sizeof(subcmd)-1); }

    int match = (strcmp(lhs, rhs) == 0);
    if (negate) match = !match;
    if (match && subcmd[0]) dispatch(subcmd);
}

/* ── Modern Windows commands ─────────────────────────────────────────────── */

static void cmd_auditpol(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || (argc >= 1 && (strcasecmp(args[0],"/?")==0 || strcasecmp(args[0],"/??")==0))) {
        printf("Usage: AuditPol command [<sub-command><options>]\r\n\r\n"
               "Commands (and sub-commands)\r\n"
               "  /get                  - Gets the current audit policy.\r\n"
               "  /set                  - Sets the audit policy.\r\n"
               "  /list                 - Lists selectable policy elements.\r\n"
               "  /backup               - Saves audit policy to a file.\r\n"
               "  /restore              - Restores audit policy from a file.\r\n"
               "  /clear                - Clears the audit policy.\r\n"
               "  /remove               - Removes per-user audit policy for a user account.\r\n"
               "  /resourceSACL         - Configures global resource SACLs.\r\n"); return;
    }
    if (strcasecmp(args[0], "/get") == 0) {
        printf("\r\nSystem audit policy\r\n"
               "Category/Subcategory                      Setting\r\n"
               "System\r\n"
               "  Security System Extension               No Auditing\r\n"
               "  System Integrity                        Success and Failure\r\n"
               "  IPsec Driver                            No Auditing\r\n"
               "  Other System Events                     Success and Failure\r\n"
               "  Security State Change                   Success\r\n"
               "Logon/Logoff\r\n"
               "  Logon                                   Success and Failure\r\n"
               "  Logoff                                  Success\r\n"
               "  Account Lockout                         Success\r\n"
               "  Special Logon                           Success\r\n"
               "Object Access\r\n"
               "  File System                             No Auditing\r\n"
               "  Registry                                No Auditing\r\n"
               "  Kernel Object                           No Auditing\r\n"
               "Privilege Use\r\n"
               "  Sensitive Privilege Use                 No Auditing\r\n"
               "  Non Sensitive Privilege Use             No Auditing\r\n"
               "Account Management\r\n"
               "  User Account Management                 Success\r\n"
               "  Computer Account Management             Success\r\n"
               "  Security Group Management               Success\r\n"
               "Policy Change\r\n"
               "  Audit Policy Change                     Success\r\n"
               "  Authentication Policy Change            Success\r\n"
               "Account Logon\r\n"
               "  Credential Validation                   Success and Failure\r\n");
    } else if (strcasecmp(args[0], "/set") == 0) {
        printf("The command was successfully executed.\r\n");
    } else if (strcasecmp(args[0], "/list") == 0) {
        printf("Category/Subcategory\r\n"
               "System\r\n  Security System Extension\r\n  System Integrity\r\n"
               "  IPsec Driver\r\n  Other System Events\r\n  Security State Change\r\n"
               "Logon/Logoff\r\n  Logon\r\n  Logoff\r\n  Account Lockout\r\n"
               "Object Access\r\n  File System\r\n  Registry\r\n  Kernel Object\r\n"
               "Privilege Use\r\n  Sensitive Privilege Use\r\n"
               "Account Management\r\n  User Account Management\r\n"
               "Policy Change\r\n  Audit Policy Change\r\n"
               "Account Logon\r\n  Credential Validation\r\n");
    } else if (strcasecmp(args[0], "/backup") == 0 || strcasecmp(args[0], "/restore") == 0 || strcasecmp(args[0], "/clear") == 0) {
        printf("The command was successfully executed.\r\n");
    } else {
        printf("The command was successfully executed.\r\n");
    }
}

static void cmd_bcdboot(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("Usage: bcdboot <source> [/l <locale>] [/s <volume-letter>] [/f <firmware type>]\r\n"
               "               [/v] [/m [{os-loader-id}]] [/addlast] [/p] [/d] [/c]\r\n\r\n"
               "  source   Specifies the location of the windows system root.\r\n"
               "  /l       Optional locale. Default is US English (en-us).\r\n"
               "  /s       Optional volume letter of the target system partition.\r\n"
               "  /f       Optional firmware type. Default is the type of the current system.\r\n"
               "  /v       Enables verbose mode.\r\n"
               "  /m       Merges objects from an existing boot entry.\r\n"
               "  /d       Preserves existing default boot entry.\r\n"
               "  /p       Preserves existing system partition.\r\n"); return;
    }
    printf("Boot files successfully created.\r\n");
}

static void cmd_certutil(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || strcasecmp(args[0], "-?")==0 || strcasecmp(args[0], "/?")==0) {
        printf("CertUtil [Options] [-dump]                      -- Dump Configuration Information or files\r\n"
               "CertUtil [Options] [-hashfile] InFile [HashAlg] -- Hash file\r\n"
               "CertUtil [Options] [-encode] InFile OutFile      -- Encode file to Base64\r\n"
               "CertUtil [Options] [-decode] InFile OutFile      -- Decode Base64 file\r\n"
               "CertUtil [Options] [-store] [CertStore [Cert]]   -- Dump certificate store\r\n"
               "CertUtil [Options] [-addstore] CertStore InFile  -- Add cert to store\r\n"
               "CertUtil [Options] [-delstore] CertStore CertId  -- Delete cert from store\r\n"
               "CertUtil [Options] [-verify] CertFile [AppPol]   -- Verify certificate\r\n"
               "CertUtil [Options] [-ping] [MaxWaitSeconds]      -- Ping ADCS\r\n"); return;
    }
    const char *sub = args[0];
    if (strcasecmp(sub,"-hashfile")==0 || strcasecmp(sub,"/hashfile")==0) {
        if (argc < 2) { printf("CertUtil: Missing file argument.\r\n"); return; }
        const char *alg = (argc >= 3) ? args[2] : "SHA1";
        /* compute real hash if possible, else fake */
        char real[MAX_PATH];
        snprintf(real, sizeof(real), "%s/%s", real_cwd, args[1]);
        FILE *f = fopen(real, "rb");
        if (!f) { printf("CertUtil: -hashfile command FAILED: 0x80070002\r\nCertUtil: The system cannot find the file specified.\r\n"); return; }
        /* Read file and produce a stable-looking fake hash */
        unsigned long h = 5381;
        int c;
        while ((c = fgetc(f)) != EOF) h = ((h << 5) + h) ^ c;
        fclose(f);
        printf("%s hash of %s:\r\n", alg, args[1]);
        /* Print 40 hex chars (SHA1-like) derived from h */
        for (int i = 0; i < 5; i++) printf("%08lx", (h * (i+1)) ^ (h >> (i*3)));
        printf("\r\nCertUtil: -hashfile command completed successfully.\r\n");
    } else if (strcasecmp(sub,"-encode")==0 || strcasecmp(sub,"/encode")==0) {
        printf("CertUtil: -encode command completed successfully.\r\n");
    } else if (strcasecmp(sub,"-decode")==0 || strcasecmp(sub,"/decode")==0) {
        printf("CertUtil: -decode command completed successfully.\r\n");
    } else if (strcasecmp(sub,"-store")==0 || strcasecmp(sub,"/store")==0) {
        const char *store = (argc >= 2) ? args[1] : "My";
        printf("================ Certificate 0 ================\r\n"
               "Serial Number: 01\r\n"
               "Issuer: CN=DESKTOP-WINTERM, DC=WORKGROUP\r\n"
               "NotBefore: 1/1/2024 12:00 AM\r\n"
               "NotAfter:  1/1/2025 12:00 AM\r\n"
               "Subject: CN=DESKTOP-WINTERM\r\n"
               "Cert Hash(sha1): aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa bb cc dd\r\n"
               "CertUtil: -store command completed successfully.\r\n");
        (void)store;
    } else if (strcasecmp(sub,"-addstore")==0 || strcasecmp(sub,"-delstore")==0 ||
               strcasecmp(sub,"-verify")==0  || strcasecmp(sub,"-ping")==0) {
        printf("CertUtil: %s command completed successfully.\r\n", sub);
    } else if (strcasecmp(sub,"-dump")==0 || sub[0]=='-' || sub[0]=='/') {
        printf("CertUtil: %s command completed successfully.\r\n", sub);
    } else {
        /* treat as file dump */
        printf("CertUtil: -dump command completed successfully.\r\n");
    }
}

static void cmd_certreq(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || strcasecmp(args[0],"/?") == 0) {
        printf("Usage:\r\n"
               "  CertReq -Submit    [Options] [RequestFileIn [CertFileOut [CertChainFileOut [FullResponseFileOut]]]]\r\n"
               "  CertReq -Retrieve  [Options] RequestId [CertFileOut [CertChainFileOut [FullResponseFileOut]]]\r\n"
               "  CertReq -New       [Options] [PolicyFileIn [RequestFileOut]]\r\n"
               "  CertReq -Accept    [Options] [CertChainFileIn | FullResponseFileIn | CertFileIn]\r\n"
               "  CertReq -Policy    [Options] [RequestFileIn [PolicyFileIn [RequestFileOut [PKCS10FileOut]]]]\r\n"
               "  CertReq -Sign      [Options] [RequestFileIn [RequestFileOut]]\r\n"
               "  CertReq -Enroll    [Options] [TemplateName]\r\n"); return;
    }
    printf("CertReq: Request submitted successfully.\r\n");
}

static void cmd_regini(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0) {
        printf("Regini [-m \\\\machinename] [-h hivefile hiveroot] [-i n] [-o outputWidth]\r\n"
               "       [-b] textFiles...\r\n\r\n"
               "  -m   Remote machine to connect to.\r\n"
               "  -h   Hive file to connect to.\r\n"
               "  -i n Indentation level.\r\n"
               "  -o   Output line width.\r\n"
               "  -b   Backwards compatible with old regini.exe.\r\n"
               "  textFiles  One or more text files with registry data.\r\n"); return;
    }
    /* skip flags, find the file */
    for (int i = 0; i < argc; i++) {
        if (args[i][0] != '-') {
            char real[MAX_PATH];
            snprintf(real, sizeof(real), "%s/%s", real_cwd, args[i]);
            FILE *f = fopen(real, "r");
            if (!f) { printf("regini: cannot open %s\r\n", args[i]); return; }
            fclose(f);
            printf("Registry updated from %s.\r\n", args[i]);
            return;
        }
    }
    printf("Registry updated successfully.\r\n");
}

static void cmd_qappsrv(const char *arg) {
    (void)arg;
    printf("Known Remote Desktop Session Host servers in domain:\r\n"
           "    DESKTOP-WINTERM\r\n");
}

static void cmd_printbrm(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || strcasecmp(args[0],"/?") == 0) {
        printf("PrintBRM [-b|-r] [-f <file>] [-d <directory>] [-?]\r\n\r\n"
               "  -b    Backup print queues and settings.\r\n"
               "  -r    Restore print queues and settings.\r\n"
               "  -f    Specify backup/restore file.\r\n"
               "  -d    Specify a print server.\r\n"); return;
    }
    int backup = 0, restore = 0;
    const char *file = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(args[i],"-b")==0) backup = 1;
        else if (strcasecmp(args[i],"-r")==0) restore = 1;
        else if (strcasecmp(args[i],"-f")==0 && i+1 < argc) file = args[++i];
    }
    if (backup)  printf("Print queue backup completed successfully%s%s.\r\n", file?" to ":"", file?file:"");
    else if (restore) printf("Print queue restore completed successfully%s%s.\r\n", file?" from ":"", file?file:"");
    else printf("The syntax of the command is incorrect.\r\n");
}

static void cmd_dispdiag(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    /* dispdiag [-out <filepath>] [-v] [-testD3D] */
    const char *outfile = "dispdiag.dat";
    for (int i = 0; i < argc; i++)
        if ((strcasecmp(args[i],"-out")==0||strcasecmp(args[i],"/out")==0) && i+1<argc)
            outfile = args[++i];
    printf("Collecting display information...\r\n"
           "Display diagnostic file saved to: %s\r\n", outfile);
}

static void cmd_netdom(const char *rest) {
    char sub[32] = {0}; int i = 0;
    const char *p = rest ? rest : "";
    while (*p && !isspace((unsigned char)*p) && i < 31) sub[i++] = toupper((unsigned char)*p++);
    while (*p == ' ') p++;
    if (!*sub || strcasecmp(sub,"HELP")==0 || strcasecmp(sub,"/?")==0) {
        printf("The syntax of this command is:\r\n\r\n"
               "NETDOM ADD | COMPUTERNAME | JOIN | MOVE | QUERY | REMOVE |\r\n"
               "       RENAME | RENAMECOMPUTER | RESET | RESETPWD | TRUST |\r\n"
               "       VERIFY [Options]\r\n"); return;
    }
    if (strcasecmp(sub,"QUERY")==0) {
        if (strstr(p,"FSMO")||strstr(p,"fsmo")) {
            printf("Schema master               DESKTOP-WINTERM\r\n"
                   "Domain naming master        DESKTOP-WINTERM\r\n"
                   "PDC                         DESKTOP-WINTERM\r\n"
                   "RID pool manager            DESKTOP-WINTERM\r\n"
                   "Infrastructure master       DESKTOP-WINTERM\r\n"
                   "The command completed successfully.\r\n");
        } else {
            printf("The command completed successfully.\r\n");
        }
    } else if (strcasecmp(sub,"JOIN")==0) {
        printf("NETDOM JOIN completed successfully.\r\n");
    } else if (strcasecmp(sub,"REMOVE")==0) {
        printf("NETDOM REMOVE completed successfully.\r\n");
    } else if (strcasecmp(sub,"VERIFY")==0) {
        printf("The secure channel from DESKTOP-WINTERM to the domain WORKGROUP has been verified. The connection is intact.\r\n"
               "The command completed successfully.\r\n");
    } else if (strcasecmp(sub,"RESETPWD")==0) {
        printf("The machine account password for the local machine has been successfully reset.\r\n"
               "The command completed successfully.\r\n");
    } else {
        printf("The command completed successfully.\r\n");
    }
}

static void cmd_netcfg(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || strcasecmp(args[0],"/?") == 0) {
        printf("NetCfg [[-v] -l <path\\filename>] [-c p|s|c] [-i <id>] [-?]\r\n"
               "       [-s <sequence>] [-u [<id>]] [-n [<id>]]\r\n\r\n"
               "  -v    Verbose mode.\r\n"
               "  -l    INF path to install from.\r\n"
               "  -c    Class of component: p=protocol, s=service, c=client.\r\n"
               "  -i    Component ID.\r\n"
               "  -s    Binding sequence.\r\n"
               "  -u    Uninstall component.\r\n"
               "  -n    Network binding.\r\n"); return;
    }
    if (strcasecmp(args[0],"-s")==0 && argc >= 2) {
        printf("Binding sequence:\r\n"
               "  ms_tcpip -> ms_ndisuio\r\n"
               "  ms_tcpip6 -> ms_ndisuio\r\n"
               "The command completed successfully.\r\n");
    } else {
        printf("The command completed successfully.\r\n");
    }
}

static void cmd_setspn(const char *rest) {
    char args[MAX_ARGS][MAX_PATH]; int argc;
    parse_args(rest, args, &argc);
    if (argc == 0 || strcasecmp(args[0],"/?") == 0) {
        printf("Usage: setspn [modifiers switch] [accountname]\r\n\r\n"
               "  -Q <spn>   Query for spn.\r\n"
               "  -A <spn>   Add spn.\r\n"
               "  -D <spn>   Delete spn.\r\n"
               "  -L         List spns for account.\r\n"
               "  -R         Reset spns for account.\r\n"
               "  -S <spn>   Add spn (verify uniqueness).\r\n"); return;
    }
    if (strcasecmp(args[0],"-L")==0 || strcasecmp(args[0],"/L")==0) {
        const char *acct = (argc >= 2) ? args[1] : "Administrator";
        printf("Registered ServicePrincipalNames for CN=%s,CN=Users,DC=WORKGROUP:\r\n"
               "\tHOST/DESKTOP-WINTERM\r\n"
               "\tHOST/DESKTOP-WINTERM.workgroup\r\n", acct);
    } else if (strcasecmp(args[0],"-Q")==0 || strcasecmp(args[0],"/Q")==0) {
        printf("Checking domain DC=WORKGROUP,DC=local\r\n\r\n"
               "No such SPN found.\r\n");
    } else if (strcasecmp(args[0],"-A")==0 || strcasecmp(args[0],"-S")==0 ||
               strcasecmp(args[0],"/A")==0 || strcasecmp(args[0],"/S")==0) {
        printf("Updated object\r\n");
    } else if (strcasecmp(args[0],"-D")==0 || strcasecmp(args[0],"/D")==0) {
        printf("Updated object\r\n");
    } else if (strcasecmp(args[0],"-R")==0 || strcasecmp(args[0],"/R")==0) {
        printf("Reset object\r\n");
    } else {
        printf("Registered ServicePrincipalNames for CN=DESKTOP-WINTERM,CN=Computers,DC=WORKGROUP:\r\n"
               "\tHOST/DESKTOP-WINTERM\r\n");
    }
}

/* ── Filesystem init ─────────────────────────────────────────────────────── */

static void mkdirp(const char *path) {
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof(tmp));
    for (int i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0755); tmp[i] = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ── Filesystem scaffolding helpers ─────────────────────────────────────── */

static void fs_mkdir(const char *rel) {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", fs_root, rel);
    mkdirp(p);
}

/* Write a stub file only if it does not already exist. */
static void fs_write(const char *rel, const char *content) {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", fs_root, rel);
    struct stat st;
    if (stat(p, &st) == 0) return;          /* already exists */
    FILE *fp = fopen(p, "w");
    if (fp) { fputs(content, fp); fclose(fp); }
}

/* ── init_fs ─────────────────────────────────────────────────────────────── */

static void init_fs(void) {

    /* ── Directory tree ─────────────────────────────────────────────────── */
    static const char *const dirs[] = {
        /* Windows core */
        "Windows",
        "Windows/System32",
        "Windows/System32/drivers",
        "Windows/System32/drivers/etc",
        "Windows/System32/config",
        "Windows/System32/spool",
        "Windows/System32/spool/PRINTERS",
        "Windows/System32/spool/drivers",
        "Windows/System32/wbem",
        "Windows/System32/wbem/Repository",
        "Windows/System32/WindowsPowerShell",
        "Windows/System32/WindowsPowerShell/v1.0",
        "Windows/System32/WindowsPowerShell/v1.0/Modules",
        "Windows/System32/OpenSSH",
        "Windows/System32/catroot",
        "Windows/System32/catroot2",
        "Windows/System32/CodeIntegrity",
        "Windows/System32/GroupPolicy",
        "Windows/System32/GroupPolicy/Machine",
        "Windows/System32/GroupPolicy/User",
        "Windows/System32/LogFiles",
        "Windows/System32/Msdtc",
        "Windows/System32/winevt",
        "Windows/System32/winevt/Logs",
        "Windows/System32/Tasks",
        "Windows/System32/Tasks/Microsoft",
        "Windows/System32/Tasks/Microsoft/Windows",
        "Windows/System32/restore",
        "Windows/System32/Macromed",
        "Windows/SysWOW64",
        "Windows/SysWOW64/WindowsPowerShell",
        "Windows/SysWOW64/WindowsPowerShell/v1.0",
        "Windows/SysWOW64/wbem",
        "Windows/Temp",
        "Windows/Logs",
        "Windows/Logs/CBS",
        "Windows/Logs/DISM",
        "Windows/Logs/MoSetup",
        "Windows/Logs/WindowsUpdate",
        "Windows/Prefetch",
        "Windows/Fonts",
        "Windows/inf",
        "Windows/Media",
        "Windows/Boot",
        "Windows/Boot/EFI",
        "Windows/Boot/MBR",
        "Windows/Boot/DVD",
        "Windows/diagnostics",
        "Windows/diagnostics/system",
        "Windows/diagnostics/index",
        "Windows/Help",
        "Windows/IME",
        "Windows/Cursors",
        "Windows/Globalization",
        "Windows/ShellNew",
        "Windows/WinSxS",
        "Windows/WinSxS/Manifests",
        "Windows/SoftwareDistribution",
        "Windows/SoftwareDistribution/Download",
        "Windows/SoftwareDistribution/DataStore",
        "Windows/ServiceProfiles",
        "Windows/ServiceProfiles/LocalService",
        "Windows/ServiceProfiles/NetworkService",
        "Windows/security",
        "Windows/security/database",
        "Windows/security/logs",
        "Windows/assembly",
        "Windows/Microsoft.NET",
        "Windows/Microsoft.NET/Framework",
        "Windows/Microsoft.NET/Framework/v4.0.30319",
        "Windows/Microsoft.NET/Framework64",
        "Windows/Microsoft.NET/Framework64/v4.0.30319",
        "Windows/PolicyDefinitions",
        "Windows/Resources",
        "Windows/Resources/Themes",
        "Windows/Resources/Themes/Aero",
        "Windows/tracing",
        "Windows/Registration",
        "Windows/repair",
        "Windows/schemas",
        "Windows/schemas/RAW",
        "Windows/twain_32",
        "Windows/addins",
        "Windows/AppReadiness",
        "Windows/bcastdvr",
        "Windows/CbsTemp",
        "Windows/debug",
        "Windows/debug/WIA",
        "Windows/LiveKernelReports",
        "Windows/Panther",
        /* Users */
        "Users",
        "Users/Administrator",
        "Users/Administrator/Desktop",
        "Users/Administrator/Documents",
        "Users/Administrator/Downloads",
        "Users/Administrator/Music",
        "Users/Administrator/Pictures",
        "Users/Administrator/Videos",
        "Users/Administrator/Favorites",
        "Users/Administrator/Contacts",
        "Users/Administrator/Searches",
        "Users/Administrator/Saved Games",
        "Users/Administrator/Links",
        "Users/Administrator/AppData",
        "Users/Administrator/AppData/Local",
        "Users/Administrator/AppData/Local/Temp",
        "Users/Administrator/AppData/Local/Microsoft",
        "Users/Administrator/AppData/Local/Microsoft/Windows",
        "Users/Administrator/AppData/Local/Microsoft/Windows/History",
        "Users/Administrator/AppData/Local/Microsoft/Windows/Temporary Internet Files",
        "Users/Administrator/AppData/Local/Microsoft/Windows/INetCache",
        "Users/Administrator/AppData/Local/Microsoft/Windows/INetCookies",
        "Users/Administrator/AppData/Local/Microsoft/Windows/Explorer",
        "Users/Administrator/AppData/Local/Microsoft/Windows/Notifications",
        "Users/Administrator/AppData/Local/Microsoft/Internet Explorer",
        "Users/Administrator/AppData/Local/Microsoft/Edge",
        "Users/Administrator/AppData/Local/Microsoft/OneDrive",
        "Users/Administrator/AppData/Local/Packages",
        "Users/Administrator/AppData/Roaming",
        "Users/Administrator/AppData/Roaming/Microsoft",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Start Menu",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Start Menu/Programs",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Accessories",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Recent",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/SendTo",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Templates",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Cookies",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Network Shortcuts",
        "Users/Administrator/AppData/Roaming/Microsoft/Windows/Printer Shortcuts",
        "Users/Administrator/AppData/Roaming/Microsoft/Internet Explorer",
        "Users/Administrator/AppData/Roaming/Microsoft/Internet Explorer/Quick Launch",
        "Users/Administrator/AppData/Roaming/Microsoft/Credentials",
        "Users/Administrator/AppData/Roaming/Microsoft/Crypto",
        "Users/Administrator/AppData/Roaming/Microsoft/MMC",
        "Users/Administrator/AppData/Roaming/Microsoft/Protect",
        "Users/Administrator/AppData/Roaming/Microsoft/SystemCertificates",
        "Users/Administrator/AppData/Roaming/Microsoft/Vault",
        "Users/Administrator/AppData/LocalLow",
        "Users/Default",
        "Users/Default/AppData/Local/Microsoft/Windows",
        "Users/Default/AppData/Roaming/Microsoft/Windows/Start Menu/Programs",
        "Users/Public",
        "Users/Public/Desktop",
        "Users/Public/Documents",
        "Users/Public/Downloads",
        "Users/Public/Music",
        "Users/Public/Pictures",
        "Users/Public/Videos",
        "Users/Public/Libraries",
        /* Program Files */
        "Program Files",
        "Program Files/Common Files",
        "Program Files/Common Files/Microsoft Shared",
        "Program Files/Common Files/Microsoft Shared/MSInfo",
        "Program Files/Common Files/Microsoft Shared/Stationery",
        "Program Files/Common Files/Services",
        "Program Files/Common Files/System",
        "Program Files/Common Files/System/Ole DB",
        "Program Files/Common Files/System/ado",
        "Program Files/Internet Explorer",
        "Program Files/Internet Explorer/Signup",
        "Program Files/Windows Defender",
        "Program Files/Windows Defender Advanced Threat Protection",
        "Program Files/Windows Mail",
        "Program Files/Windows Media Player",
        "Program Files/Windows Media Player/Skins",
        "Program Files/Windows NT",
        "Program Files/Windows NT/Accessories",
        "Program Files/Windows NT/MSAgent",
        "Program Files/Windows NT/TableTextService",
        "Program Files/Windows Photo Viewer",
        "Program Files/Windows Sidebar",
        "Program Files/WindowsPowerShell",
        "Program Files/WindowsPowerShell/Modules",
        "Program Files/Microsoft Update Health Tools",
        "Program Files/7-Zip",
        "Program Files/WinRAR",
        "Program Files/Notepad++",
        "Program Files/Git",
        "Program Files/Git/bin",
        "Program Files/Git/usr/bin",
        "Program Files/Python312",
        "Program Files/Python312/Lib",
        "Program Files/Python312/Scripts",
        "Program Files/Mozilla Firefox",
        "Program Files/Google",
        "Program Files/Google/Chrome",
        "Program Files/Google/Chrome/Application",
        "Program Files (x86)",
        "Program Files (x86)/Common Files",
        "Program Files (x86)/Common Files/Microsoft Shared",
        "Program Files (x86)/Internet Explorer",
        "Program Files (x86)/Windows Media Player",
        "Program Files (x86)/Microsoft",
        "Program Files (x86)/Microsoft/Edge",
        /* ProgramData */
        "ProgramData",
        "ProgramData/Microsoft",
        "ProgramData/Microsoft/Windows",
        "ProgramData/Microsoft/Windows/Start Menu",
        "ProgramData/Microsoft/Windows/Start Menu/Programs",
        "ProgramData/Microsoft/Windows/Start Menu/Programs/Accessories",
        "ProgramData/Microsoft/Windows/Start Menu/Programs/Administrative Tools",
        "ProgramData/Microsoft/Windows/Start Menu/Programs/Startup",
        "ProgramData/Microsoft/Windows/Start Menu/Programs/System Tools",
        "ProgramData/Microsoft/Windows/WER",
        "ProgramData/Microsoft/Windows/WER/ReportQueue",
        "ProgramData/Microsoft/Windows/WER/ReportArchive",
        "ProgramData/Microsoft/Windows/WER/Temp",
        "ProgramData/Microsoft/Windows/WindowsUpdate",
        "ProgramData/Microsoft/Windows/DeliveryOptimization",
        "ProgramData/Microsoft/Windows Defender",
        "ProgramData/Microsoft/Windows Defender/Scans",
        "ProgramData/Microsoft/Windows Defender/Quarantine",
        "ProgramData/Microsoft/Windows Defender/Platform",
        "ProgramData/Microsoft/Windows Defender/Definition Updates",
        "ProgramData/Microsoft/Diagnosis",
        "ProgramData/Microsoft/MF",
        "ProgramData/Microsoft/Network",
        "ProgramData/Microsoft/NetFramework",
        "ProgramData/Microsoft/Search",
        "ProgramData/Microsoft/Search/Data",
        "ProgramData/Microsoft/Search/Data/Applications",
        "ProgramData/Microsoft/Search/Data/Applications/Windows",
        "ProgramData/Microsoft/Event Viewer",
        "ProgramData/Microsoft/Crypto",
        "ProgramData/Microsoft/Crypto/RSA",
        "ProgramData/Microsoft/Crypto/RSA/MachineKeys",
        "ProgramData/Microsoft/SystemCertificates",
        "ProgramData/Microsoft/User Account Pictures",
        "ProgramData/USOShared",
        "ProgramData/USOShared/Logs",
        "ProgramData/Package Cache",
        /* Recycle Bin stub */
        "$Recycle.Bin",
        "$Recycle.Bin/S-1-5-21-0000000000-0000000000-000000000-500",
        NULL
    };

    for (int i = 0; dirs[i]; i++) fs_mkdir(dirs[i]);

    /* ── Stub files ──────────────────────────────────────────────────────── */

    /* Windows root */
    fs_write("Windows/win.ini",
        "; for 16-bit app support\r\n"
        "[fonts]\r\n"
        "[extensions]\r\n"
        "[mci extensions]\r\n"
        "[files]\r\n"
        "[Mail]\r\n"
        "MAPI=1\r\n");

    fs_write("Windows/system.ini",
        "[386Enh]\r\n"
        "woafont=dosapp.fon\r\n"
        "EGA80WOA.FON=EGA80WOA.FON\r\n"
        "EGA40WOA.FON=EGA40WOA.FON\r\n"
        "CGA80WOA.FON=CGA80WOA.FON\r\n"
        "CGA40WOA.FON=CGA40WOA.FON\r\n"
        "[drivers]\r\n"
        "wave=mmdrv.dll\r\n"
        "timer=timer.drv\r\n"
        "[mci]\r\n");

    fs_write("Windows/regedit.exe", "MZ[stub: regedit.exe]\r\n");
    fs_write("Windows/notepad.exe", "MZ[stub: notepad.exe]\r\n");
    fs_write("Windows/explorer.exe", "MZ[stub: explorer.exe]\r\n");
    fs_write("Windows/HelpPane.exe", "MZ[stub: HelpPane.exe]\r\n");
    fs_write("Windows/write.exe", "MZ[stub: write.exe]\r\n");
    fs_write("Windows/twain_32.dll", "MZ[stub: twain_32.dll]\r\n");

    /* Boot */
    fs_write("Windows/Boot/EFI/bootmgfw.efi",
        "[stub EFI bootloader - Windows Boot Manager]\r\n");
    fs_write("Windows/Boot/MBR/bootmgr",
        "[stub MBR bootloader]\r\n");

    /* System32 — executables */
    static const char *const sys32_exes[] = {
        "cmd.exe","powershell.exe","powershell_ise.exe",
        "conhost.exe","conime.exe",
        "notepad.exe","calc.exe","mspaint.exe","wordpad.exe","write.exe",
        "regedit.exe","regedt32.exe",
        "taskmgr.exe","tasklist.exe","taskhost.exe","taskhostw.exe",
        "svchost.exe","services.exe","lsass.exe","lsm.exe",
        "winlogon.exe","wininit.exe","csrss.exe","smss.exe",
        "explorer.exe","dwm.exe","sihost.exe","fontdrvhost.exe",
        "spoolsv.exe","sppsvc.exe",
        "msiexec.exe","mshta.exe","msiexec.exe",
        "rundll32.exe","regsvr32.exe","regsvcs.exe","regasm.exe",
        "sc.exe","net.exe","net1.exe","netstat.exe","netsh.exe",
        "ping.exe","tracert.exe","pathping.exe","arp.exe","nslookup.exe",
        "ipconfig.exe","route.exe","hostname.exe","getmac.exe",
        "whoami.exe","runas.exe",
        "attrib.exe","cacls.exe","icacls.exe","takeown.exe",
        "xcopy.exe","robocopy.exe","expand.exe","compact.exe",
        "chkdsk.exe","chkntfs.exe","diskpart.exe","fsutil.exe",
        "format.com","label.exe","subst.exe","vol.exe",
        "defrag.exe","dfrg.msc",
        "eventvwr.exe","eventvwr.msc",
        "msconfig.exe","msinfo32.exe","mmc.exe",
        "devmgmt.msc","compmgmt.msc","services.msc","taskschd.msc",
        "secpol.msc","gpedit.msc","lusrmgr.msc","fsmgmt.msc",
        "diskmgmt.msc","perfmon.msc","perfmon.exe",
        "control.exe","appwiz.cpl","desk.cpl","inetcpl.cpl",
        "intl.cpl","joy.cpl","main.cpl","mmsys.cpl","ncpa.cpl",
        "powercfg.cpl","sysdm.cpl","telephon.cpl","timedate.cpl",
        "wuauclt.exe","wusa.exe","usoclient.exe",
        "schtasks.exe","at.exe","timeout.exe","waitfor.exe",
        "shutdown.exe","logoff.exe",
        "cipher.exe","certutil.exe","certreq.exe",
        "bcdedit.exe","bcdboot.exe","bootrec.exe","bootsect.exe",
        "winpe.exe","reagentc.exe","recenv.exe",
        "dism.exe","pkgmgr.exe",
        "wmic.exe","wbemtest.exe",
        "systeminfo.exe","systemreset.exe",
        "sfc.exe","verifier.exe","sigverif.exe",
        "auditpol.exe","secedit.exe","gpupdate.exe","gpresult.exe",
        "nltest.exe","netdom.exe","setspn.exe","dsquery.exe",
        "reg.exe","regini.exe",
        "findstr.exe","find.exe","sort.exe","more.com",
        "clip.exe","choice.exe","where.exe",
        "fc.exe","comp.exe","replace.exe",
        "tree.com","xcopy.exe",
        "makecab.exe","expand.exe","extrac32.exe",
        "bitsadmin.exe","robocopy.exe",
        "ftp.exe","finger.exe","telnet.exe","tftp.exe",
        "ssh.exe","scp.exe","sftp.exe",
        "curl.exe","tar.exe",
        "dxdiag.exe","dxcap.exe",
        "cscript.exe","wscript.exe",
        "mstsc.exe","mstsc.exe",
        "odbcad32.exe","odbcconf.exe",
        "printui.exe","printbrm.exe",
        "osk.exe","magnify.exe","narrator.exe","utilman.exe",
        "snippingtool.exe","snippingtool.exe","SnippingTool.exe",
        "charmap.exe","cleanmgr.exe",
        "mblctr.exe","xpsrchvw.exe",
        "psr.exe","xwizard.exe","rekeywiz.exe",
        "sdclt.exe","recdisc.exe",
        "iexplore.exe","microsoftedgecp.exe",
        "MicrosoftEdgeUpdate.exe",
        "wermgr.exe","werconv.exe",
        "vssadmin.exe","vshadow.exe",
        "winrm.cmd","winrs.exe",
        "wpeutil.exe","wpeinit.exe",
        "tskill.exe","taskkill.exe",
        "qprocess.exe","quser.exe","qwinsta.exe","rwinsta.exe",
        "query.exe","msg.exe","shadow.exe","change.exe",
        "tscon.exe","tsdiscon.exe",
        "sxstrace.exe","typeperf.exe",
        "lodctr.exe","unlodctr.exe",
        "wecutil.exe",
        "checknetisolation.exe",
        "dispdiag.exe",
        "rpcping.exe","rpcss.dll",
        "mtstocom.exe",
        "klist.exe",
        "kerberos.dll",
        "netcfg.exe",
        "setver.exe",
        "share.exe",
        "pcalua.exe","pcaui.exe",
        "iexpress.exe",
        "infdefaultinstall.exe",
        "cttune.exe","hdwwiz.exe","credwiz.exe","eudcedit.exe",
        "netplwiz.exe","colorcpl.exe",
        "mblctr.exe","dfsvc.exe",
        "MRT.exe","MRTHelper.exe",
        "OneDriveSetup.exe",
        NULL
    };
    for (int i = 0; sys32_exes[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/System32/%s", sys32_exes[i]);
        char content[256];
        snprintf(content, sizeof(content), "MZ[stub: %s]\r\n", sys32_exes[i]);
        fs_write(rel, content);
    }

    /* System32 — DLLs */
    static const char *const sys32_dlls[] = {
        "ntdll.dll","kernel32.dll","kernelbase.dll","user32.dll",
        "gdi32.dll","gdi32full.dll","win32u.dll",
        "advapi32.dll","sechost.dll","msvcrt.dll",
        "rpcrt4.dll","combase.dll","ole32.dll","oleaut32.dll",
        "shell32.dll","shlwapi.dll","shcore.dll",
        "comdlg32.dll","comctl32.dll","uxtheme.dll",
        "ws2_32.dll","wsock32.dll","winhttp.dll","wininet.dll",
        "urlmon.dll","ieframe.dll","iertutil.dll",
        "crypt32.dll","cryptbase.dll","cryptsp.dll","cryptdll.dll",
        "bcrypt.dll","bcryptprimitives.dll","ncrypt.dll",
        "setupapi.dll","cfgmgr32.dll","devobj.dll",
        "wtsapi32.dll","wintrust.dll","imagehlp.dll","dbghelp.dll",
        "psapi.dll","pdh.dll","powrprof.dll",
        "iphlpapi.dll","dnsapi.dll","rasapi32.dll",
        "netapi32.dll","samcli.dll","samlib.dll","samsrv.dll",
        "logoncli.dll","seclogon.dll","netlogon.dll",
        "lsasrv.dll","msasn1.dll","kerberos.dll","msv1_0.dll",
        "schannel.dll","sspicli.dll","secur32.dll",
        "wldap32.dll","activeds.dll","adsldp.dll","adsldpc.dll",
        "mswsock.dll","fwpuclnt.dll","nsi.dll",
        "mlang.dll","normaliz.dll","usp10.dll",
        "d3d11.dll","d3d12.dll","d3dcompiler_47.dll",
        "dxgi.dll","dxgidebug.dll","dcomp.dll",
        "opengl32.dll","glu32.dll",
        "wintypes.dll","wmvcore.dll","mfplat.dll","mf.dll",
        "msmpeg2vdec.dll","msmpeg2adec.dll",
        "avrt.dll","mmdevapi.dll","audioses.dll","dsound.dll",
        "ntmarta.dll","authz.dll","wbemcomn.dll","wbemprox.dll",
        "wbemdisp.dll","fastprox.dll","wmiutils.dll",
        "vdsbas.dll","vds.exe","dismapi.dll",
        "msi.dll","mspatcha.dll","cabinet.dll",
        "msxml6.dll","msxml3.dll",
        "version.dll","api-ms-win-core-path-l1-1-0.dll",
        "ucrtbase.dll","vcruntime140.dll","msvcp140.dll",
        "clbcatq.dll","clbcatex.dll",
        "profapi.dll","userenv.dll",
        "winmm.dll","winmmbase.dll",
        "imm32.dll","msimtf.dll","cicero.dll",
        "sppc.dll","slc.dll","sppobjs.dll",
        "wer.dll","faultrep.dll","werconcur.dll",
        "ntasn1.dll","scesrv.dll","winspool.drv",
        NULL
    };
    for (int i = 0; sys32_dlls[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/System32/%s", sys32_dlls[i]);
        char content[256];
        snprintf(content, sizeof(content), "MZ[stub DLL: %s]\r\n", sys32_dlls[i]);
        fs_write(rel, content);
    }

    /* drivers/etc */
    fs_write("Windows/System32/drivers/etc/hosts",
        "# Copyright (c) 1993-2009 Microsoft Corp.\r\n"
        "#\r\n"
        "# This is a sample HOSTS file used by Microsoft TCP/IP for Windows.\r\n"
        "#\r\n"
        "# Entries are listed in the format:\r\n"
        "#   <IP address>    <hostname>\r\n"
        "#\r\n"
        "127.0.0.1       localhost\r\n"
        "::1             localhost\r\n");

    fs_write("Windows/System32/drivers/etc/networks",
        "loopback  127\r\n");

    fs_write("Windows/System32/drivers/etc/protocol",
        "# Internet (IP) protocols\r\n"
        "ip    0   IP\r\n"
        "icmp  1   ICMP\r\n"
        "tcp   6   TCP\r\n"
        "udp  17   UDP\r\n");

    fs_write("Windows/System32/drivers/etc/services",
        "# Well-known port assignments\r\n"
        "ftp-data  20/tcp\r\n"
        "ftp       21/tcp\r\n"
        "ssh       22/tcp\r\n"
        "telnet    23/tcp\r\n"
        "smtp      25/tcp\r\n"
        "http      80/tcp  www\r\n"
        "https    443/tcp\r\n"
        "rdp     3389/tcp\r\n"
        "winrm   5985/tcp\r\n"
        "winrms  5986/tcp\r\n");

    /* System32/config — registry hive stubs */
    fs_write("Windows/System32/config/SAM",      "[SAM hive stub]\r\n");
    fs_write("Windows/System32/config/SECURITY",  "[SECURITY hive stub]\r\n");
    fs_write("Windows/System32/config/SOFTWARE",  "[SOFTWARE hive stub]\r\n");
    fs_write("Windows/System32/config/SYSTEM",    "[SYSTEM hive stub]\r\n");
    fs_write("Windows/System32/config/DEFAULT",   "[DEFAULT hive stub]\r\n");
    fs_write("Windows/System32/config/COMPONENTS","[COMPONENTS hive stub]\r\n");
    fs_write("Windows/System32/config/BCD-Template","[BCD-Template stub]\r\n");

    /* PowerShell profile */
    fs_write("Windows/System32/WindowsPowerShell/v1.0/profile.ps1",
        "# Windows PowerShell profile\r\n");
    fs_write("Windows/System32/WindowsPowerShell/v1.0/powershell.exe.config",
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<configuration>\r\n"
        "  <startup useLegacyV2RuntimeActivationPolicy=\"true\">\r\n"
        "    <supportedRuntime version=\"v4.0.30319\"/>\r\n"
        "  </startup>\r\n"
        "</configuration>\r\n");

    /* wbem */
    fs_write("Windows/System32/wbem/WBEMCORE.DLL",  "MZ[stub]\r\n");
    fs_write("Windows/System32/wbem/WMI.DLL",       "MZ[stub]\r\n");

    /* winevt log stubs */
    static const char *const evtlogs[] = {
        "Application.evtx","Security.evtx","System.evtx",
        "Setup.evtx","ForwardedEvents.evtx",
        "Microsoft-Windows-PowerShell%4Operational.evtx",
        "Microsoft-Windows-Sysmon%4Operational.evtx",
        "Microsoft-Windows-TaskScheduler%4Operational.evtx",
        "Microsoft-Windows-WinRM%4Operational.evtx",
        NULL
    };
    for (int i = 0; evtlogs[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/System32/winevt/Logs/%s", evtlogs[i]);
        char content[256];
        snprintf(content, sizeof(content),
            "ElfFile[stub EVTX: %s]\r\n", evtlogs[i]);
        fs_write(rel, content);
    }

    /* Windows/Logs */
    fs_write("Windows/Logs/CBS/CBS.log",
        "2024-01-15 08:42:11, Info                  CBS    Loaded Servicing Stack v10.0.19041.1\r\n"
        "2024-01-15 08:42:12, Info                  CBS    Starting TrustedInstaller initialization\r\n"
        "2024-01-15 08:42:13, Info                  CBS    Trusted Installer initialized and started\r\n");

    fs_write("Windows/Logs/DISM/dism.log",
        "2024-01-15 08:40:00, Info                  DISM   DISM.EXE: <-(Logging initialized)\r\n"
        "2024-01-15 08:40:01, Info                  DISM   Image session initialized\r\n");

    /* Windows Media */
    static const char *const media_files[] = {
        "Windows Logon.wav","Windows Logoff.wav",
        "Windows Startup.wav","Windows Shutdown.wav",
        "Windows Error.wav","Windows Exclamation.wav",
        "Windows Notify.wav","Windows Ding.wav",
        "tada.wav","chord.wav","chimes.wav",
        "recycle.wav","Speech Misrecognition.wav",
        NULL
    };
    for (int i = 0; media_files[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/Media/%s", media_files[i]);
        char content[256];
        snprintf(content, sizeof(content), "RIFF[stub WAV: %s]\r\n", media_files[i]);
        fs_write(rel, content);
    }

    /* Windows/Fonts */
    static const char *const fonts[] = {
        "arial.ttf","arialbd.ttf","ariali.ttf","arialbi.ttf",
        "times.ttf","timesbd.ttf","timesi.ttf","timesbi.ttf",
        "cour.ttf","courbd.ttf","couri.ttf","courbi.ttf",
        "verdana.ttf","verdanab.ttf","verdanai.ttf","verdanaz.ttf",
        "tahoma.ttf","tahomabd.ttf",
        "segoeui.ttf","segoeuib.ttf","segoeuii.ttf","segoeuiz.ttf",
        "consola.ttf","consolab.ttf","consolai.ttf","consolaz.ttf",
        "calibri.ttf","calibrib.ttf","calibrii.ttf","calibriz.ttf",
        "cambria.ttc","cambriai.ttf","cambriab.ttf","cambriaz.ttf",
        "comic.ttf","comicbd.ttf",
        "impact.ttf","lucon.ttf","msyh.ttc","simhei.ttf",
        "wingding.ttf","webdings.ttf","symbol.ttf",
        "cour.fon","courf.fon","vgaoem.fon","dosapp.fon",
        NULL
    };
    for (int i = 0; fonts[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/Fonts/%s", fonts[i]);
        char content[256];
        snprintf(content, sizeof(content), "[stub font: %s]\r\n", fonts[i]);
        fs_write(rel, content);
    }

    /* Windows/inf */
    static const char *const inf_files[] = {
        "cpu.inf","disk.inf","display.inf","keyboard.inf",
        "machine.inf","monitor.inf","mouse.inf","msmouse.inf",
        "net.inf","netloop.inf","pci.inf","scsi.inf",
        "syssetup.inf","usbstor.inf","usbport.inf",
        "wdmaud.inf","wsnmp32.inf",
        NULL
    };
    for (int i = 0; inf_files[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/inf/%s", inf_files[i]);
        char content[256];
        snprintf(content, sizeof(content),
            "; Device setup information\r\n[Version]\r\nSignature=\"$Windows NT$\"\r\n"
            "; stub: %s\r\n", inf_files[i]);
        fs_write(rel, content);
    }

    /* Users/Administrator desktop / documents */
    fs_write("Users/Administrator/Desktop/desktop.ini",
        "[.ShellClassInfo]\r\n"
        "LocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-21769\r\n"
        "IconResource=%SystemRoot%\\system32\\imageres.dll,-183\r\n");

    fs_write("Users/Administrator/Documents/desktop.ini",
        "[.ShellClassInfo]\r\n"
        "LocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-21770\r\n"
        "IconResource=%SystemRoot%\\system32\\imageres.dll,-112\r\n");

    fs_write("Users/Administrator/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/desktop.ini",
        "[.ShellClassInfo]\r\n"
        "LocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-21786\r\n");

    /* NTUSER.DAT stubs */
    fs_write("Users/Administrator/NTUSER.DAT",  "[NTUSER.DAT hive stub]\r\n");
    fs_write("Users/Administrator/ntuser.ini",
        "[MRUList]\r\n"
        "ProfileLoadTimeLow=0\r\n"
        "ProfileLoadTimeHigh=0\r\n");
    fs_write("Users/Default/NTUSER.DAT", "[NTUSER.DAT hive stub]\r\n");

    /* Program Files stubs */
    fs_write("Program Files/Internet Explorer/iexplore.exe",
        "MZ[stub: Internet Explorer 11]\r\n");
    fs_write("Program Files/Internet Explorer/ieinstal.exe",
        "MZ[stub]\r\n");
    fs_write("Program Files/Windows Defender/MsMpEng.exe",
        "MZ[stub: Windows Defender engine]\r\n");
    fs_write("Program Files/Windows Defender/MpCmdRun.exe",
        "MZ[stub]\r\n");
    fs_write("Program Files/Windows Media Player/wmplayer.exe",
        "MZ[stub: Windows Media Player]\r\n");
    fs_write("Program Files/Windows NT/Accessories/wordpad.exe",
        "MZ[stub: WordPad]\r\n");
    fs_write("Program Files/WindowsPowerShell/Modules/readme.txt",
        "PowerShell module directory.\r\n");
    fs_write("Program Files/7-Zip/7z.exe",
        "MZ[stub: 7-Zip 23.01]\r\n");
    fs_write("Program Files/7-Zip/7-zip.dll",
        "MZ[stub]\r\n");
    fs_write("Program Files/Notepad++/notepad++.exe",
        "MZ[stub: Notepad++ 8.6.2]\r\n");
    fs_write("Program Files/Git/bin/git.exe",
        "MZ[stub: Git 2.44.0]\r\n");
    fs_write("Program Files/Python312/python.exe",
        "MZ[stub: Python 3.12.2]\r\n");
    fs_write("Program Files/Python312/python3.exe",
        "MZ[stub]\r\n");
    fs_write("Program Files/Python312/pythonw.exe",
        "MZ[stub]\r\n");
    fs_write("Program Files/Python312/Scripts/pip.exe",
        "MZ[stub: pip 24.0]\r\n");
    fs_write("Program Files/Mozilla Firefox/firefox.exe",
        "MZ[stub: Firefox 123.0]\r\n");
    fs_write("Program Files/Google/Chrome/Application/chrome.exe",
        "MZ[stub: Google Chrome 122.0]\r\n");
    fs_write("Program Files/WinRAR/WinRAR.exe",
        "MZ[stub: WinRAR 6.24]\r\n");

    /* ProgramData */
    fs_write("ProgramData/Microsoft/Windows/Start Menu/Programs/desktop.ini",
        "[.ShellClassInfo]\r\n"
        "LocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-21782\r\n");

    fs_write("ProgramData/Microsoft/Windows Defender/Platform/README.txt",
        "Windows Defender platform definitions directory.\r\n");

    /* .NET */
    fs_write("Windows/Microsoft.NET/Framework64/v4.0.30319/mscorwks.dll",
        "MZ[stub: .NET CLR 4.0.30319]\r\n");
    fs_write("Windows/Microsoft.NET/Framework64/v4.0.30319/clr.dll",
        "MZ[stub]\r\n");
    fs_write("Windows/Microsoft.NET/Framework/v4.0.30319/mscorwks.dll",
        "MZ[stub: .NET CLR 4.0.30319 (x86)]\r\n");
    fs_write("Windows/Microsoft.NET/Framework/v4.0.30319/clr.dll",
        "MZ[stub]\r\n");

    /* SoftwareDistribution / Windows Update */
    fs_write("Windows/SoftwareDistribution/DataStore/DataStore.edb",
        "[stub: Windows Update DataStore]\r\n");
    fs_write("Windows/SoftwareDistribution/Download/readme.txt",
        "Windows Update download cache.\r\n");

    /* Prefetch stubs */
    static const char *const prefetch[] = {
        "CMD.EXE-089B7E16.pf","EXPLORER.EXE-1C93C2E1.pf",
        "POWERSHELL.EXE-04F90371.pf","TASKMGR.EXE-8D2B392A.pf",
        "NOTEPAD.EXE-E5B1B2C0.pf","SVCHOST.EXE-B7C40329.pf",
        "MSIEXEC.EXE-9B8E6D0F.pf","WUAUCLT.EXE-2A49B6F1.pf",
        NULL
    };
    for (int i = 0; prefetch[i]; i++) {
        char rel[MAX_PATH];
        snprintf(rel, sizeof(rel), "Windows/Prefetch/%s", prefetch[i]);
        char content[256];
        snprintf(content, sizeof(content),
            "SCCA[stub prefetch: %s]\r\n", prefetch[i]);
        fs_write(rel, content);
    }

    /* Recycle Bin desktop.ini */
    fs_write("$Recycle.Bin/desktop.ini",
        "[.ShellClassInfo]\r\n"
        "CLSID={645FF040-5081-101B-9F08-00AA002F954E}\r\n"
        "LocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-8964\r\n");

    /* repair / recovery */
    fs_write("Windows/repair/SAM",      "[SAM repair stub]\r\n");
    fs_write("Windows/repair/SECURITY", "[SECURITY repair stub]\r\n");
    fs_write("Windows/repair/SOFTWARE", "[SOFTWARE repair stub]\r\n");
    fs_write("Windows/repair/SYSTEM",   "[SYSTEM repair stub]\r\n");
    fs_write("Windows/repair/default",  "[default repair stub]\r\n");

    /* pagefile / hiberfil stubs at root */
    fs_write("pagefile.sys",  "[Windows paging file stub]\r\n");
    fs_write("hiberfil.sys",  "[Windows hibernation file stub]\r\n");
    fs_write("swapfile.sys",  "[Windows swap file stub]\r\n");
    fs_write("bootmgr",       "[Windows Boot Manager stub]\r\n");
    fs_write("BOOTNXT",       "[BOOTNXT stub]\r\n");
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */


/* ═══════════════════════════════════════════════════════════════════════════
 * REAL EXECUTION LAYER  (injected by patch.py)
 * Maps Windows CMD commands → Linux equivalents via system() / popen().
 * Returns 1 if the command was handled for real, 0 to fall through to stub.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <sys/wait.h>

/* Run a Linux shell command, printing its output line by line with \r\n */
static void real_run(const char *sh) {
    FILE *p = popen(sh, "r");
    if (!p) { printf("(exec failed)\r\n"); return; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) {
        /* strip trailing newline then emit \r\n */
        buf[strcspn(buf, "\n")] = '\0';
        printf("%s\r\n", buf);
    }
    pclose(p);
}

/* Escape a single argument for a POSIX shell single-quote string */
static void shell_quote(const char *s, char *out, size_t outsz) {
    size_t oi = 0;
    out[oi++] = '\'';
    for (; *s && oi < outsz - 4; s++) {
        if (*s == '\'') { out[oi++]='\''; out[oi++]='\\'; out[oi++]='\''; out[oi++]='\''; }
        else out[oi++] = *s;
    }
    out[oi++] = '\'';
    out[oi] = '\0';
}

/* Convert win path arg to a real path (uses module-level helpers) */
static void arg_to_real(const char *arg, char *out) {
    if (!arg || !*arg) { strncpy(out, real_cwd, MAX_PATH); return; }
    resolve_path(arg, out);
}

static int real_exec(const char *cmd, const char *rest) {

    char sh[MAX_INPUT * 2];
    char rp[MAX_PATH], rp2[MAX_PATH];
    char qa[MAX_PATH*2], qb[MAX_PATH*2];

    /* ── Filesystem & navigation ─────────────────────────────────────────── */

    if (strcmp(cmd, "DIR") == 0) {
        arg_to_real(*rest ? rest : NULL, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "ls -la --time-style=long-iso %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "TREE") == 0) {
        arg_to_real(*rest ? rest : NULL, rp);
        shell_quote(rp, qa, sizeof(qa));
        /* prefer the 'tree' binary, fall back to find */
        snprintf(sh, sizeof(sh),
            "command -v tree >/dev/null 2>&1 && tree %s || find %s -print 2>&1",
            qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "TYPE") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "cat %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "COPY") == 0) {
        /* Parse two args */
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "cp %s %s 2>&1 && echo '        1 file(s) copied.'", qa, qb);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "MOVE") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "mv %s %s 2>&1 && echo '        1 file(s) moved.'", qa, qb);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "DEL") == 0 || strcmp(cmd, "ERASE") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "rm -f %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "MD") == 0 || strcmp(cmd, "MKDIR") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "mkdir -p %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "RD") == 0 || strcmp(cmd, "RMDIR") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "rmdir %s 2>&1 || rm -rf %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "REN") == 0 || strcmp(cmd, "RENAME") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "mv %s %s 2>&1", qa, qb);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "XCOPY") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "cp -r %s %s 2>&1", qa, qb);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "ROBOCOPY") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "rsync -av %s/ %s 2>&1", rp, rp2);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "ATTRIB") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "stat --format='%%A %%n' %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "COMPACT") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "du -sh %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "WHERE") == 0) {
        if (!*rest) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "which %s 2>/dev/null || find / -name %s 2>/dev/null | head -10", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "SUBST") == 0) {
        /* No real drive letters on Linux; list mounts instead */
        real_run("mount | grep -v 'proc\\|sys\\|dev\\|run' 2>&1");
        return 1;
    }

    /* ── Text / search ───────────────────────────────────────────────────── */

    if (strcmp(cmd, "FIND") == 0) {
        /* FIND "string" file  → grep string file */
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 1) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        shell_quote(args[0], qa, sizeof(qa));
        if (argc >= 2) {
            arg_to_real(args[1], rp);
            shell_quote(rp, qb, sizeof(qb));
            snprintf(sh, sizeof(sh), "grep -n %s %s 2>&1", qa, qb);
        } else {
            snprintf(sh, sizeof(sh), "grep -rn %s . 2>&1", qa);
        }
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "FINDSTR") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 1) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        shell_quote(args[0], qa, sizeof(qa));
        if (argc >= 2) {
            arg_to_real(args[1], rp);
            shell_quote(rp, qb, sizeof(qb));
            snprintf(sh, sizeof(sh), "grep -En %s %s 2>&1", qa, qb);
        } else {
            snprintf(sh, sizeof(sh), "grep -Ern %s . 2>&1", qa);
        }
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "SORT") == 0) {
        if (*rest) {
            arg_to_real(rest, rp);
            shell_quote(rp, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "sort %s 2>&1", qa);
        } else {
            snprintf(sh, sizeof(sh), "sort 2>&1");
        }
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "MORE") == 0) {
        if (*rest) {
            arg_to_real(rest, rp);
            shell_quote(rp, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "cat %s 2>&1", qa);
            real_run(sh);
        }
        return 1;
    }
    if (strcmp(cmd, "CLIP") == 0) {
        /* Write stdin to xclip/xsel/pbcopy if available */
        real_run("command -v xclip >/dev/null 2>&1 && echo '(clipboard: use xclip)' || echo 'CLIP: clipboard not available in this terminal.'");
        return 1;
    }
    if (strcmp(cmd, "FC") == 0 || strcmp(cmd, "COMP") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 2) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp); arg_to_real(args[1], rp2);
        shell_quote(rp, qa, sizeof(qa)); shell_quote(rp2, qb, sizeof(qb));
        snprintf(sh, sizeof(sh), "diff %s %s 2>&1", qa, qb);
        real_run(sh);
        return 1;
    }

    /* ── Process management ──────────────────────────────────────────────── */

    if (strcmp(cmd, "TASKLIST") == 0) {
        real_run("ps aux 2>&1");
        return 1;
    }
    if (strcmp(cmd, "TASKKILL") == 0) {
        /* TASKKILL /PID 1234  or  /IM name */
        const char *p = rest;
        while (*p == ' ') p++;
        if (strncasecmp(p, "/PID", 4) == 0) {
            p += 4; while (*p == ' ') p++;
            snprintf(sh, sizeof(sh), "kill %s 2>&1", p);
            real_run(sh);
        } else if (strncasecmp(p, "/IM", 3) == 0) {
            p += 3; while (*p == ' ') p++;
            shell_quote(p, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "pkill -f %s 2>&1", qa);
            real_run(sh);
        } else {
            printf("Invalid syntax. Use /PID <pid> or /IM <name>\r\n");
        }
        return 1;
    }
    if (strcmp(cmd, "START") == 0) {
        if (!*rest) return 1;
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "%s &", rest);
        system(sh);
        return 1;
    }
    if (strcmp(cmd, "TIMEOUT") == 0) {
        int secs = atoi(rest);
        if (secs > 0) {
            snprintf(sh, sizeof(sh), "sleep %d", secs);
            system(sh);
        }
        return 1;
    }
    if (strcmp(cmd, "PAUSE") == 0) {
        printf("Press any key to continue . . . ");
        fflush(stdout);
        getchar();
        printf("\r\n");
        return 1;
    }
    if (strcmp(cmd, "TSKILL") == 0) {
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "kill %s 2>&1", qa);
        real_run(sh);
        return 1;
    }

    /* ── Disk & filesystem info ───────────────────────────────────────────── */

    if (strcmp(cmd, "CHKDSK") == 0) {
        real_run("df -h 2>&1 && echo '--- inode usage ---' && df -i 2>&1");
        return 1;
    }
    if (strcmp(cmd, "DEFRAG") == 0) {
        real_run("df -h 2>&1");
        printf("(Defragmentation not applicable on Linux filesystems.)\r\n");
        return 1;
    }
    if (strcmp(cmd, "DISKPART") == 0) {
        real_run("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>&1");
        return 1;
    }
    if (strcmp(cmd, "FSUTIL") == 0) {
        real_run("stat -f . 2>&1 || df -T . 2>&1");
        return 1;
    }
    if (strcmp(cmd, "VOL") == 0) {
        real_run("df -h . 2>&1");
        return 1;
    }
    if (strcmp(cmd, "FORMAT") == 0) {
        printf("FORMAT is not permitted in emulation mode.\r\n");
        return 1;
    }
    if (strcmp(cmd, "DISM") == 0) {
        real_run("uname -a 2>&1");
        return 1;
    }
    if (strcmp(cmd, "VSSADMIN") == 0) {
        real_run("df -h 2>&1");
        return 1;
    }

    /* ── System info ─────────────────────────────────────────────────────── */

    if (strcmp(cmd, "SYSTEMINFO") == 0) {
        real_run("uname -a && lscpu 2>/dev/null | head -20 && free -h 2>/dev/null");
        return 1;
    }
    if (strcmp(cmd, "WINVER") == 0) {
        real_run("uname -r 2>&1");
        return 1;
    }
    if (strcmp(cmd, "WMIC") == 0) {
        if (strncasecmp(rest, "cpu", 3) == 0)
            real_run("lscpu 2>&1");
        else if (strncasecmp(rest, "memorychip", 10) == 0 || strncasecmp(rest, "os", 2) == 0)
            real_run("free -h 2>&1 && uname -a 2>&1");
        else if (strncasecmp(rest, "diskdrive", 9) == 0 || strncasecmp(rest, "logicaldisk", 11) == 0)
            real_run("lsblk 2>&1");
        else if (strncasecmp(rest, "process", 7) == 0)
            real_run("ps aux 2>&1 | head -30");
        else
            real_run("uname -a 2>&1");
        return 1;
    }
    if (strcmp(cmd, "DRIVERQUERY") == 0) {
        real_run("lsmod 2>&1 | head -40");
        return 1;
    }
    if (strcmp(cmd, "WHOAMI") == 0) {
        real_run("whoami 2>&1 && id 2>&1");
        return 1;
    }
    if (strcmp(cmd, "HOSTNAME") == 0) {
        real_run("hostname 2>&1");
        return 1;
    }

    /* ── Network ─────────────────────────────────────────────────────────── */

    if (strcmp(cmd, "IPCONFIG") == 0) {
        real_run("ip addr show 2>/dev/null || ifconfig 2>/dev/null");
        return 1;
    }
    if (strcmp(cmd, "PING") == 0) {
        if (!*rest) { printf("Bad command or file name.\r\n"); return 1; }
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "ping -c 4 %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "TRACERT") == 0) {
        if (!*rest) { printf("Bad command or file name.\r\n"); return 1; }
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "traceroute %s 2>/dev/null || tracepath %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "NSLOOKUP") == 0) {
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "nslookup %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "NETSTAT") == 0) {
        real_run("ss -tulpn 2>/dev/null || netstat -tulpn 2>&1");
        return 1;
    }
    if (strcmp(cmd, "ARP") == 0) {
        real_run("arp -n 2>/dev/null || ip neigh show 2>&1");
        return 1;
    }
    if (strcmp(cmd, "ROUTE") == 0) {
        real_run("ip route show 2>/dev/null || route -n 2>&1");
        return 1;
    }
    if (strcmp(cmd, "NET") == 0 || strcmp(cmd, "NET1") == 0) {
        if (strncasecmp(rest, "user", 4) == 0)
            real_run("cat /etc/passwd 2>&1 | cut -d: -f1");
        else if (strncasecmp(rest, "localgroup", 10) == 0)
            real_run("cat /etc/group 2>&1 | cut -d: -f1");
        else if (strncasecmp(rest, "share", 5) == 0)
            real_run("df -h 2>&1");
        else if (strncasecmp(rest, "start", 5) == 0 || strncasecmp(rest, "stop", 4) == 0 || strncasecmp(rest, "use", 3) == 0)
            real_run("systemctl list-units --type=service --state=running 2>&1 | head -20");
        else
            real_run("ip addr show 2>&1");
        return 1;
    }
    if (strcmp(cmd, "NETSH") == 0) {
        real_run("ip addr show 2>&1 && ip route show 2>&1");
        return 1;
    }
    if (strcmp(cmd, "FTP") == 0) {
        if (*rest) {
            shell_quote(rest, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "ftp %s 2>&1", qa);
            real_run(sh);
        } else {
            printf("ftp> (No host specified)\r\n");
        }
        return 1;
    }
    if (strcmp(cmd, "CURL") == 0 || strcmp(cmd, "WGET") == 0) {
        if (!*rest) { printf("URL required.\r\n"); return 1; }
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "curl -L %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "BITSADMIN") == 0) {
        real_run("command -v wget >/dev/null 2>&1 && echo 'wget available' || echo 'wget/curl available for downloads'");
        return 1;
    }

    /* ── Users & security ────────────────────────────────────────────────── */

    if (strcmp(cmd, "WHOAMI") == 0) {
        real_run("whoami && id 2>&1");
        return 1;
    }
    if (strcmp(cmd, "CACLS") == 0 || strcmp(cmd, "ICACLS") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "ls -la %s 2>&1 && stat %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "TAKEOWN") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "chown $(whoami) %s 2>&1", qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "CIPHER") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "file %s 2>&1 && ls -la %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "RUNAS") == 0) {
        /* Map to sudo */
        const char *p = strstr(rest, "/user:");
        if (p) p += 6; else p = rest;
        shell_quote(p, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "sudo -u %s true 2>&1 || echo 'sudo: command failed'", qa);
        real_run(sh);
        return 1;
    }

    /* ── Registry (simulated with env) ──────────────────────────────────── */

    if (strcmp(cmd, "REG") == 0 || strcmp(cmd, "REGEDIT") == 0 || strcmp(cmd, "REGEDT32") == 0) {
        if (strncasecmp(rest, "query", 5) == 0)
            real_run("env 2>&1 | sort");
        else if (strncasecmp(rest, "add", 3) == 0)
            printf("Registry add simulated (no persistent registry on Linux).\r\n");
        else if (strncasecmp(rest, "delete", 6) == 0)
            printf("Registry delete simulated.\r\n");
        else if (strncasecmp(rest, "export", 6) == 0)
            real_run("env 2>&1");
        else
            real_run("env 2>&1 | sort | head -30");
        return 1;
    }

    /* ── Scheduling & services ───────────────────────────────────────────── */

    if (strcmp(cmd, "SCHTASKS") == 0 || strcmp(cmd, "AT") == 0) {
        real_run("crontab -l 2>&1 || echo '(no crontab for this user)'");
        return 1;
    }
    if (strcmp(cmd, "SC") == 0) {
        if (strncasecmp(rest, "query", 5) == 0 || !*rest)
            real_run("systemctl list-units --type=service 2>&1 | head -30");
        else if (strncasecmp(rest, "start", 5) == 0) {
            const char *svc = rest + 5; while (*svc == ' ') svc++;
            shell_quote(svc, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "systemctl start %s 2>&1", qa);
            real_run(sh);
        } else if (strncasecmp(rest, "stop", 4) == 0) {
            const char *svc = rest + 4; while (*svc == ' ') svc++;
            shell_quote(svc, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "systemctl stop %s 2>&1", qa);
            real_run(sh);
        } else {
            real_run("systemctl list-units --type=service 2>&1 | head -20");
        }
        return 1;
    }

    /* ── Environment ─────────────────────────────────────────────────────── */

    if (strcmp(cmd, "SET") == 0 && !*rest) {
        /* augment with real env */
        real_run("env 2>&1 | sort");
        return 1;
    }

    /* ── Hashing & certs ─────────────────────────────────────────────────── */

    if (strcmp(cmd, "CERTUTIL") == 0) {
        if (strncasecmp(rest, "-hashfile", 9) == 0) {
            char args[MAX_ARGS][MAX_PATH]; int argc;
            parse_args(rest + 9, args, &argc);
            if (argc >= 1) {
                arg_to_real(args[0], rp);
                shell_quote(rp, qa, sizeof(qa));
                const char *alg = (argc >= 2) ? args[1] : "SHA256";
                char algl[16]; strncpy(algl, alg, 15); algl[15]='\0';
                for (int ci = 0; algl[ci]; ci++) algl[ci] = tolower((unsigned char)algl[ci]);
                snprintf(sh, sizeof(sh), "%ssum %s 2>&1", algl, qa);
                real_run(sh);
            }
        } else {
            real_run("openssl version 2>&1");
        }
        return 1;
    }

    /* ── Compression ─────────────────────────────────────────────────────── */

    if (strcmp(cmd, "EXPAND") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 1) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp);
        shell_quote(rp, qa, sizeof(qa));
        if (argc >= 2) {
            arg_to_real(args[1], rp2);
            shell_quote(rp2, qb, sizeof(qb));
            snprintf(sh, sizeof(sh), "expand %s > %s 2>&1", qa, qb);
        } else {
            snprintf(sh, sizeof(sh), "expand %s 2>&1", qa);
        }
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "MAKECAB") == 0) {
        printf("MAKECAB: use 'zip' or 'tar' on Linux.\r\n");
        return 1;
    }
    if (strcmp(cmd, "EXTRAC32") == 0) {
        char args[MAX_ARGS][MAX_PATH]; int argc;
        parse_args(rest, args, &argc);
        if (argc < 1) { printf("The syntax of the command is incorrect.\r\n"); return 1; }
        arg_to_real(args[0], rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "unzip %s 2>&1 || cabextract %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }

    /* ── Printing ────────────────────────────────────────────────────────── */

    if (strcmp(cmd, "PRINT") == 0) {
        arg_to_real(rest, rp);
        shell_quote(rp, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "lp %s 2>&1 || lpr %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }

    /* ── Batch / scripting helpers ───────────────────────────────────────── */

    if (strcmp(cmd, "DOSKEY") == 0) {
        real_run("history 2>&1 | tail -20");
        return 1;
    }
    if (strcmp(cmd, "CHOICE") == 0) {
        printf("[Y/N]? ");
        fflush(stdout);
        int c = getchar();
        printf("\r\n");
        (void)c;
        return 1;
    }

    /* ── Diagnostics ─────────────────────────────────────────────────────── */

    if (strcmp(cmd, "SFC") == 0) {
        real_run("dpkg -l 2>/dev/null | head -20 || rpm -qa 2>/dev/null | head -20 || echo 'Package manager not detected.'");
        return 1;
    }
    if (strcmp(cmd, "BCDEDIT") == 0) {
        real_run("cat /proc/cmdline 2>&1 && cat /boot/grub/grub.cfg 2>/dev/null | head -20");
        return 1;
    }
    if (strcmp(cmd, "MSCONFIG") == 0) {
        real_run("systemctl list-unit-files --type=service 2>&1 | head -30");
        return 1;
    }
    if (strcmp(cmd, "PERFMON") == 0 || strcmp(cmd, "RESMON") == 0) {
        real_run("top -bn1 2>&1 | head -25");
        return 1;
    }
    if (strcmp(cmd, "MEM") == 0) {
        real_run("free -h 2>&1 && cat /proc/meminfo 2>&1 | head -15");
        return 1;
    }
    if (strcmp(cmd, "TYPEPERF") == 0) {
        real_run("vmstat 1 3 2>&1");
        return 1;
    }
    if (strcmp(cmd, "WINSAT") == 0) {
        real_run("sysbench cpu run 2>/dev/null || echo 'sysbench not installed; run: apt install sysbench'");
        return 1;
    }

    /* ── PowerShell passthrough ──────────────────────────────────────────── */

    if (strcmp(cmd, "POWERSHELL") == 0 || strcmp(cmd, "PWSH") == 0) {
        if (*rest) {
            /* -Command "..." passthrough */
            const char *p = rest;
            if (strncasecmp(p, "-command", 8) == 0) { p += 8; while (*p == ' ') p++; }
            shell_quote(p, qa, sizeof(qa));
            snprintf(sh, sizeof(sh), "pwsh -Command %s 2>&1 || powershell %s 2>&1", qa, qa);
            real_run(sh);
        } else {
            real_run("pwsh 2>/dev/null || echo 'PowerShell not installed: apt install powershell'");
        }
        return 1;
    }

    /* ── Miscellaneous ───────────────────────────────────────────────────── */

    if (strcmp(cmd, "TITLE") == 0) {
        if (*rest) printf("\033]0;%s\007", rest);
        return 1;
    }
    if (strcmp(cmd, "ASSOC") == 0) {
        /* use stub – no real file-assoc table on Linux */
        return 0;
    }
    if (strcmp(cmd, "FTYPE") == 0) {
        real_run("xdg-mime query default application/octet-stream 2>&1 || echo 'xdg-mime not available'");
        return 1;
    }
    if (strcmp(cmd, "PATHPING") == 0) {
        if (!*rest) { printf("Usage: PATHPING <host>\r\n"); return 1; }
        shell_quote(rest, qa, sizeof(qa));
        snprintf(sh, sizeof(sh), "mtr --report --report-cycles 5 %s 2>&1 || traceroute %s 2>&1", qa, qa);
        real_run(sh);
        return 1;
    }
    if (strcmp(cmd, "SXSTRACE") == 0 || strcmp(cmd, "LOGMAN") == 0 ||
        strcmp(cmd, "WEVTUTIL") == 0 || strcmp(cmd, "WECUTIL") == 0) {
        real_run("journalctl -n 50 2>&1 || dmesg | tail -20 2>&1");
        return 1;
    }
    if (strcmp(cmd, "AUDITPOL") == 0) {
        real_run("ausearch -m USER_LOGIN 2>/dev/null | tail -20 || last 2>&1 | head -20");
        return 1;
    }
    if (strcmp(cmd, "GPUPDATE") == 0 || strcmp(cmd, "GPRESULT") == 0) {
        printf("Group Policy not applicable on Linux. Showing /etc/environment:\r\n");
        real_run("cat /etc/environment 2>&1");
        return 1;
    }
    if (strcmp(cmd, "MSIEXEC") == 0) {
        printf("MSIEXEC: use 'dpkg -i <pkg.deb>' or 'rpm -i <pkg.rpm>' on Linux.\r\n");
        return 1;
    }
    if (strcmp(cmd, "WINRM") == 0 || strcmp(cmd, "WINRS") == 0) {
        printf("WinRM not available on Linux. Use SSH instead.\r\n");
        return 1;
    }
    if (strcmp(cmd, "NLTEST") == 0) {
        real_run("hostname -f 2>&1 && cat /etc/hosts 2>&1 | head -10");
        return 1;
    }

    return 0; /* not handled – fall through to stub */
}

/* ═══════════════════════════════════════════════════════════════════════════ */

static void dispatch(const char *input) {
    char expanded[MAX_INPUT * 2];
    expand_vars(input, expanded, sizeof(expanded));

    const char *p = expanded;
    while (*p == ' ') p++;
    if (!*p || *p == ':') return;

    char cmd[64] = {0};
    const char *rest = p;
    int ci = 0;
    while (*rest && !isspace((unsigned char)*rest) && ci < 63) cmd[ci++] = *rest++;
    while (*rest == ' ') rest++;
    for (int i = 0; cmd[i]; i++) cmd[i] = toupper((unsigned char)cmd[i]);

    
    /* ── Real execution layer (injected by patch.py) ── */
    if (real_exec(cmd, rest)) return;

if      (strcmp(cmd, "EXIT")        == 0) { printf("\r\n"); exit(0); }
    else if (strcmp(cmd, "VER")         == 0) cmd_ver();
    else if (strcmp(cmd, "CLS")         == 0) cmd_cls();
    else if (strcmp(cmd, "ECHO")        == 0) cmd_echo(rest);
    else if (strcmp(cmd, "DIR")         == 0) cmd_dir(*rest ? rest : NULL);
    else if (strcmp(cmd, "CD")          == 0 || strcmp(cmd, "CHDIR")   == 0) cmd_cd(*rest ? rest : NULL);
    else if (strcmp(cmd, "MKDIR")       == 0 || strcmp(cmd, "MD")      == 0) cmd_mkdir(rest);
    else if (strcmp(cmd, "RMDIR")       == 0 || strcmp(cmd, "RD")      == 0) cmd_rmdir(rest);
    else if (strcmp(cmd, "DEL")         == 0 || strcmp(cmd, "ERASE")   == 0) cmd_del(rest);
    else if (strcmp(cmd, "TYPE")        == 0) cmd_type(rest);
    else if (strcmp(cmd, "COPY")        == 0) cmd_copy(rest);
    else if (strcmp(cmd, "REN")         == 0 || strcmp(cmd, "RENAME")  == 0) cmd_ren(rest);
    else if (strcmp(cmd, "MOVE")        == 0) cmd_move(rest);
    else if (strcmp(cmd, "SET")         == 0) cmd_set(*rest ? rest : NULL);
    else if (strcmp(cmd, "DATE")        == 0) cmd_date_cmd();
    else if (strcmp(cmd, "TIME")        == 0) cmd_time_cmd();
    else if (strcmp(cmd, "HELP")        == 0) cmd_help();
    else if (strcmp(cmd, "TREE")        == 0) cmd_tree(*rest ? rest : NULL);
    else if (strcmp(cmd, "IF")          == 0) cmd_if(rest);
    else if (strcmp(cmd, "REM")         == 0) { /* comment */ }
    else if (strcmp(cmd, "PAUSE")       == 0) {
        printf("Press any key to continue . . ."); fflush(stdout); getchar(); printf("\r\n");
    }
    /* New commands */
    else if (strcmp(cmd, "ASSOC")       == 0) cmd_assoc(*rest ? rest : NULL);
    else if (strcmp(cmd, "ATTRIB")      == 0) cmd_attrib(rest);
    else if (strcmp(cmd, "CALL")        == 0) cmd_call(rest);
    else if (strcmp(cmd, "CHKDSK")      == 0) cmd_chkdsk(rest);
    else if (strcmp(cmd, "CHKNTFS")     == 0) cmd_chkntfs(rest);
    else if (strcmp(cmd, "COLOR")       == 0) cmd_color(*rest ? rest : NULL);
    else if (strcmp(cmd, "COMP")        == 0) cmd_comp(rest);
    else if (strcmp(cmd, "COMPACT")     == 0) cmd_compact(rest);
    else if (strcmp(cmd, "CONVERT")     == 0) cmd_convert(rest);
    else if (strcmp(cmd, "DISKPART")    == 0) cmd_diskpart();
    else if (strcmp(cmd, "DISM")        == 0) cmd_dism(rest);
    else if (strcmp(cmd, "DOSKEY")      == 0) cmd_doskey(rest);
    else if (strcmp(cmd, "DRIVERQUERY") == 0) cmd_driverquery();
    else if (strcmp(cmd, "ENDLOCAL")    == 0) cmd_endlocal();
    else if (strcmp(cmd, "FC")          == 0) cmd_fc(rest);
    else if (strcmp(cmd, "FIND")        == 0) cmd_find(rest);
    else if (strcmp(cmd, "FINDSTR")     == 0) cmd_findstr(rest);
    else if (strcmp(cmd, "FOR")         == 0) cmd_for_cmd(rest);
    else if (strcmp(cmd, "FORMAT")      == 0) cmd_format(rest);
    else if (strcmp(cmd, "FTYPE")       == 0) cmd_ftype(*rest ? rest : NULL);
    else if (strcmp(cmd, "GPRESULT")    == 0) cmd_gpresult(rest);
    else if (strcmp(cmd, "HOSTNAME")    == 0) cmd_hostname();
    else if (strcmp(cmd, "ICACLS")      == 0) cmd_icacls(rest);
    else if (strcmp(cmd, "IPCONFIG")    == 0) cmd_ipconfig(*rest ? rest : NULL);
    else if (strcmp(cmd, "LABEL")       == 0) cmd_label(*rest ? rest : NULL);
    else if (strcmp(cmd, "MKLINK")      == 0) cmd_mklink(rest);
    else if (strcmp(cmd, "NET")         == 0) cmd_net(rest);
    else if (strcmp(cmd, "NETSTAT")     == 0) cmd_netstat(*rest ? rest : NULL);
    else if (strcmp(cmd, "PATH")        == 0) cmd_path(*rest ? rest : NULL);
    else if (strcmp(cmd, "PATHPING")    == 0) cmd_pathping(rest);
    else if (strcmp(cmd, "PING")        == 0) cmd_ping(rest);
    else if (strcmp(cmd, "POPD")        == 0) cmd_popd();
    else if (strcmp(cmd, "PRINT")       == 0) cmd_print(rest);
    else if (strcmp(cmd, "PROMPT")      == 0) cmd_prompt(*rest ? rest : NULL);
    else if (strcmp(cmd, "PUSHD")       == 0) cmd_pushd(*rest ? rest : NULL);
    else if (strcmp(cmd, "REG")         == 0) cmd_reg(rest);
    else if (strcmp(cmd, "REGEDIT")     == 0) cmd_regedit();
    else if (strcmp(cmd, "REPLACE")     == 0) cmd_replace(rest);
    else if (strcmp(cmd, "ROBOCOPY")    == 0) cmd_robocopy(rest);
    else if (strcmp(cmd, "SC")          == 0) cmd_sc(rest);
    else if (strcmp(cmd, "SCHTASKS")    == 0) cmd_schtasks(rest);
    else if (strcmp(cmd, "SETLOCAL")    == 0) cmd_setlocal();
    else if (strcmp(cmd, "SHIFT")       == 0) cmd_shift(rest);
    else if (strcmp(cmd, "SHUTDOWN")    == 0) cmd_shutdown(rest);
    else if (strcmp(cmd, "SORT")        == 0) cmd_sort(rest);
    else if (strcmp(cmd, "START")       == 0) cmd_start(rest);
    else if (strcmp(cmd, "SUBST")       == 0) cmd_subst(rest);
    else if (strcmp(cmd, "SYSTEMINFO")  == 0) cmd_systeminfo();
    else if (strcmp(cmd, "TAKEOWN")     == 0) cmd_takeown(rest);
    else if (strcmp(cmd, "TASKKILL")    == 0) cmd_taskkill(rest);
    else if (strcmp(cmd, "TASKLIST")    == 0) cmd_tasklist(rest);
    else if (strcmp(cmd, "TIMEOUT")     == 0) cmd_timeout(rest);
    else if (strcmp(cmd, "TITLE")       == 0) cmd_title(rest);
    else if (strcmp(cmd, "TRACERT")     == 0) cmd_tracert(rest);
    else if (strcmp(cmd, "VOL")         == 0) cmd_vol();
    else if (strcmp(cmd, "WHERE")       == 0) cmd_where(rest);
    else if (strcmp(cmd, "WHOAMI")      == 0) cmd_whoami(*rest ? rest : NULL);
    else if (strcmp(cmd, "WINVER")      == 0) cmd_winver();
    else if (strcmp(cmd, "WMIC")        == 0) cmd_wmic(rest);
    else if (strcmp(cmd, "XCOPY")       == 0) cmd_xcopy(rest);
    else if (strcmp(cmd, "ARP")         == 0) cmd_arp(*rest ? rest : NULL);
    else if (strcmp(cmd, "AT")          == 0) cmd_at(rest);
    else if (strcmp(cmd, "BCDEDIT")     == 0) cmd_bcdedit(rest);
    else if (strcmp(cmd, "BREAK")       == 0) cmd_break(*rest ? rest : NULL);
    else if (strcmp(cmd, "CHOICE")      == 0) cmd_choice(rest);
    else if (strcmp(cmd, "CIPHER")      == 0) cmd_cipher(rest);
    else if (strcmp(cmd, "CLIP")        == 0) cmd_clip();
    else if (strcmp(cmd, "DEFRAG")      == 0) cmd_defrag(*rest ? rest : NULL);
    else if (strcmp(cmd, "EXPAND")      == 0) cmd_expand(rest);
    else if (strcmp(cmd, "FSUTIL")      == 0) cmd_fsutil(rest);
    else if (strcmp(cmd, "GOTO")        == 0) cmd_goto(rest);
    else if (strcmp(cmd, "MORE")        == 0) cmd_more(*rest ? rest : NULL);
    else if (strcmp(cmd, "MSIEXEC")     == 0) cmd_msiexec(rest);
    else if (strcmp(cmd, "MSCONFIG")    == 0) cmd_msconfig();
    else if (strcmp(cmd, "NETSH")       == 0) cmd_netsh(rest);
    else if (strcmp(cmd, "NSLOOKUP")    == 0) cmd_nslookup(rest);
    else if (strcmp(cmd, "PKTMON")      == 0) cmd_pktmon(rest);
    else if (strcmp(cmd, "RECOVER")     == 0) cmd_recover(rest);
    else if (strcmp(cmd, "REGSVR32")    == 0) cmd_regsvr32(rest);
    else if (strcmp(cmd, "RUNAS")       == 0) cmd_runas(rest);
    else if (strcmp(cmd, "SFC")         == 0) cmd_sfc(rest);
    else if (strcmp(cmd, "VERIFY")      == 0) cmd_verify(*rest ? rest : NULL);
    else if (strcmp(cmd, "WAITFOR")     == 0) cmd_waitfor(rest);
    else if (strcmp(cmd, "WEVTUTIL")    == 0) cmd_wevtutil(rest);
    else if (strcmp(cmd, "GETMAC")      == 0) cmd_getmac(rest);
    else if (strcmp(cmd, "GPUPDATE")    == 0) cmd_gpupdate(rest);
    else if (strcmp(cmd, "OPENFILES")   == 0) cmd_openfiles(rest);
    else if (strcmp(cmd, "POWERCFG")    == 0) cmd_powercfg(rest);
    else if (strcmp(cmd, "RUNDLL32")    == 0) cmd_rundll32(rest);
    else if (strcmp(cmd, "TZUTIL")      == 0) cmd_tzutil(rest);
    else if (strcmp(cmd, "W32TM")       == 0) cmd_w32tm(rest);
    else if (strcmp(cmd, "WBADMIN")     == 0) cmd_wbadmin(rest);
    else if (strcmp(cmd, "WUSA")        == 0) cmd_wusa(rest);
    else if (strcmp(cmd, "BOOTREC")     == 0) cmd_bootrec(rest);
    else if (strcmp(cmd, "SECEDIT")     == 0) cmd_secedit(rest);
    else if (strcmp(cmd, "LOGMAN")      == 0) cmd_logman(rest);
    else if (strcmp(cmd, "REAGENTC")    == 0) cmd_reagentc(rest);
    else if (strcmp(cmd, "DXDIAG")      == 0) cmd_dxdiag(rest);
    /* Added commands */
    else if (strcmp(cmd, "APPEND")      == 0) cmd_append(*rest ? rest : NULL);
    else if (strcmp(cmd, "CACLS")       == 0) cmd_cacls(rest);
    else if (strcmp(cmd, "CHCP")        == 0) cmd_chcp(*rest ? rest : NULL);
    else if (strcmp(cmd, "CMDKEY")      == 0) cmd_cmdkey(*rest ? rest : NULL);
    else if (strcmp(cmd, "CSCRIPT")     == 0) cmd_cscript(*rest ? rest : NULL);
    else if (strcmp(cmd, "WSCRIPT")     == 0) cmd_wscript(*rest ? rest : NULL);
    else if (strcmp(cmd, "DISKCOMP")    == 0) cmd_diskcomp(rest);
    else if (strcmp(cmd, "DISKCOPY")    == 0) cmd_diskcopy(rest);
    else if (strcmp(cmd, "FINGER")      == 0) cmd_finger(*rest ? rest : NULL);
    else if (strcmp(cmd, "FTP")         == 0) cmd_ftp(*rest ? rest : NULL);
    else if (strcmp(cmd, "MEM")         == 0) cmd_mem(*rest ? rest : NULL);
    else if (strcmp(cmd, "MODE")        == 0) cmd_mode(*rest ? rest : NULL);
    else if (strcmp(cmd, "MSTSC")       == 0) cmd_mstsc(*rest ? rest : NULL);
    else if (strcmp(cmd, "NLTEST")      == 0) cmd_nltest(*rest ? rest : NULL);
    else if (strcmp(cmd, "QPROCESS")    == 0) cmd_qprocess(*rest ? rest : NULL);
    else if (strcmp(cmd, "QUERY")       == 0) cmd_query(rest);
    else if (strcmp(cmd, "ROUTE")       == 0) cmd_route(rest);
    else if (strcmp(cmd, "RPCPING")     == 0) cmd_rpcping(*rest ? rest : NULL);
    else if (strcmp(cmd, "SETVER")      == 0) cmd_setver(*rest ? rest : NULL);
    else if (strcmp(cmd, "SHARE")       == 0) cmd_share(*rest ? rest : NULL);
    else if (strcmp(cmd, "SXSTRACE")    == 0) cmd_sxstrace(*rest ? rest : NULL);
    else if (strcmp(cmd, "TYPEPERF")    == 0) cmd_typeperf(*rest ? rest : NULL);
    else if (strcmp(cmd, "UNLODCTR")    == 0) cmd_unlodctr(*rest ? rest : NULL);
    else if (strcmp(cmd, "LODCTR")      == 0) cmd_lodctr(*rest ? rest : NULL);
    else if (strcmp(cmd, "VSSADMIN")    == 0) cmd_vssadmin(rest);
    else if (strcmp(cmd, "WECUTIL")     == 0) cmd_wecutil(rest);
    else if (strcmp(cmd, "WINRM")       == 0) cmd_winrm(rest);
    else if (strcmp(cmd, "WINRS")       == 0) cmd_winrs(*rest ? rest : NULL);
    else if (strcmp(cmd, "WPEUTIL")     == 0) cmd_wpeutil(*rest ? rest : NULL);
    else if (strcmp(cmd, "NET1")        == 0) cmd_net1(rest);
    else if (strcmp(cmd, "PERFMON")     == 0) cmd_perfmon(*rest ? rest : NULL);
    else if (strcmp(cmd, "IEXPRESS")    == 0) cmd_iexpress(*rest ? rest : NULL);
    else if (strcmp(cmd, "CONTROL")     == 0) cmd_control(*rest ? rest : NULL);
    else if (strcmp(cmd, "MMC")         == 0) cmd_mmc(*rest ? rest : NULL);
    else if (strcmp(cmd, "MSPAINT")     == 0) cmd_mspaint(*rest ? rest : NULL);
    else if (strcmp(cmd, "NOTEPAD")     == 0) cmd_notepad(*rest ? rest : NULL);
    else if (strcmp(cmd, "CALC")        == 0) cmd_calc(*rest ? rest : NULL);
    else if (strcmp(cmd, "TASKMGR")     == 0) cmd_taskmgr(*rest ? rest : NULL);
    else if (strcmp(cmd, "EXPLORER")    == 0) cmd_explorer(*rest ? rest : NULL);
    else if (strcmp(cmd, "CHARMAP")     == 0) cmd_charmap(*rest ? rest : NULL);
    else if (strcmp(cmd, "CLEANMGR")    == 0) cmd_cleanmgr(*rest ? rest : NULL);
    else if (strcmp(cmd, "EVENTVWR")    == 0) cmd_eventvwr(*rest ? rest : NULL);
    else if (strcmp(cmd, "MSINFO32")    == 0) cmd_msinfo32(*rest ? rest : NULL);
    else if (strcmp(cmd, "DEVMGMT.MSC") == 0) cmd_devmgmt(*rest ? rest : NULL);
    else if (strcmp(cmd, "COMPMGMT.MSC")== 0) cmd_compmgmt(*rest ? rest : NULL);
    else if (strcmp(cmd, "SECPOL.MSC")  == 0) cmd_secpol(*rest ? rest : NULL);
    else if (strcmp(cmd, "GPEDIT.MSC")  == 0) cmd_gpedit(*rest ? rest : NULL);
    else if (strcmp(cmd, "LUSRMGR.MSC") == 0) cmd_lusrmgr(*rest ? rest : NULL);
    else if (strcmp(cmd, "ODBCAD32")    == 0) cmd_odbcad32(*rest ? rest : NULL);
    else if (strcmp(cmd, "PRINTUI")     == 0) cmd_printui(*rest ? rest : NULL);
    else if (strcmp(cmd, "OSK")         == 0) cmd_osk(*rest ? rest : NULL);
    else if (strcmp(cmd, "MAGNIFY")     == 0) cmd_magnify(*rest ? rest : NULL);
    else if (strcmp(cmd, "NARRATOR")    == 0) cmd_narrator(*rest ? rest : NULL);
    else if (strcmp(cmd, "SNIPPINGTOOL")== 0) cmd_snippet(*rest ? rest : NULL);
    else if (strcmp(cmd, "WRITE")       == 0) cmd_write(*rest ? rest : NULL);
    else if (strcmp(cmd, "WORDPAD")     == 0) cmd_write(*rest ? rest : NULL);
    else if (strcmp(cmd, "QUSER")       == 0) cmd_quser(*rest ? rest : NULL);
    else if (strcmp(cmd, "QWINSTA")     == 0) cmd_qwinsta(*rest ? rest : NULL);
    else if (strcmp(cmd, "RWINSTA")     == 0) cmd_rwinsta(*rest ? rest : NULL);
    else if (strcmp(cmd, "LOGOFF")      == 0) cmd_logoff(*rest ? rest : NULL);
    else if (strcmp(cmd, "MSG")         == 0) cmd_msg(*rest ? rest : NULL);
    else if (strcmp(cmd, "SHADOW")      == 0) cmd_shadow(*rest ? rest : NULL);
    else if (strcmp(cmd, "TSDISCON")    == 0) cmd_tsdiscon(*rest ? rest : NULL);
    else if (strcmp(cmd, "TSCON")       == 0) cmd_tscon(*rest ? rest : NULL);
    else if (strcmp(cmd, "CHANGE")      == 0) cmd_change(rest);
    else if (strcmp(cmd, "TSKILL")      == 0) cmd_tskill(*rest ? rest : NULL);
    else if (strcmp(cmd, "FLATTEMP")    == 0) cmd_flattemp(*rest ? rest : NULL);
    else if (strcmp(cmd, "PKGMGR")      == 0) cmd_pkgmgr(*rest ? rest : NULL);
    else if (strcmp(cmd, "PCALUA")      == 0) cmd_pcalua(*rest ? rest : NULL);
    else if (strcmp(cmd, "BITSADMIN")   == 0) cmd_bitsadmin(rest);
    else if (strcmp(cmd, "MAKECAB")     == 0) cmd_makecab(rest);
    else if (strcmp(cmd, "EXTRAC32")    == 0) cmd_extrac32(rest);
    else if (strcmp(cmd, "PSR")         == 0) cmd_psr(*rest ? rest : NULL);
    else if (strcmp(cmd, "XWIZARD")     == 0) cmd_xwizard(*rest ? rest : NULL);
    else if (strcmp(cmd, "REKEYWIZ")    == 0) cmd_rekeywiz(*rest ? rest : NULL);
    else if (strcmp(cmd, "SDCLT")       == 0) cmd_sdclt(*rest ? rest : NULL);
    else if (strcmp(cmd, "RECDISC")     == 0) cmd_recdisc(*rest ? rest : NULL);
    else if (strcmp(cmd, "CHECKNETISOLATION") == 0) cmd_checknetisolation(*rest ? rest : NULL);
    else if (strcmp(cmd, "DFSVC")       == 0) cmd_dfsvc();
    else if (strcmp(cmd, "CTTUNE")      == 0) cmd_cttune();
    else if (strcmp(cmd, "HDWWIZ")      == 0) cmd_hdwwiz();
    else if (strcmp(cmd, "CREDWIZ")     == 0) cmd_credwiz();
    else if (strcmp(cmd, "EUDCEDIT")    == 0) cmd_eudcedit();
    else if (strcmp(cmd, "FSMGMT.MSC")  == 0) cmd_fsmgmt();
    else if (strcmp(cmd, "SYSDM.CPL")   == 0) cmd_sysdm();
    else if (strcmp(cmd, "NETPLWIZ")    == 0) cmd_netplwiz();
    else if (strcmp(cmd, "TIMEDATE.CPL")== 0) cmd_timedate();
    else if (strcmp(cmd, "COLORCPL")    == 0) cmd_colorcpl();
    else if (strcmp(cmd, "MBLCTR")      == 0) cmd_mblctr();
    /* Modern Windows additions */
    else if (strcmp(cmd, "AUDITPOL")    == 0) cmd_auditpol(rest);
    else if (strcmp(cmd, "BCDBOOT")     == 0) cmd_bcdboot(rest);
    else if (strcmp(cmd, "CERTUTIL")    == 0) cmd_certutil(rest);
    else if (strcmp(cmd, "CERTREQ")     == 0) cmd_certreq(rest);
    else if (strcmp(cmd, "REGINI")      == 0) cmd_regini(rest);
    else if (strcmp(cmd, "QAPPSRV")     == 0) cmd_qappsrv(*rest ? rest : NULL);
    else if (strcmp(cmd, "PRINTBRM")    == 0) cmd_printbrm(rest);
    else if (strcmp(cmd, "DISPDIAG")    == 0) cmd_dispdiag(rest);
    else if (strcmp(cmd, "NETDOM")      == 0) cmd_netdom(rest);
    else if (strcmp(cmd, "NETCFG")      == 0) cmd_netcfg(rest);
    else if (strcmp(cmd, "SETSPN")      == 0) cmd_setspn(rest);
    else {
        /* Try .bat in cwd */
        char bat[MAX_PATH];
        snprintf(bat, sizeof(bat), "%s/%s.bat", real_cwd, cmd);
        FILE *bf = fopen(bat, "r");
        if (bf) {
            char line[MAX_INPUT];
            while (fgets(line, sizeof(line), bf)) {
                line[strcspn(line, "\r\n")] = 0;
                dispatch(line);
            }
            fclose(bf);
        } else {
            printf("'%s' is not recognized as an internal or external command,\r\n"
                   "operable program or batch file.\r\n", cmd);
        }
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static void sigint_handler(int sig) {
    (void)sig;
    /* Print ^C like real cmd.exe, then re-show prompt on next iteration */
    printf("\r\n");
    fflush(stdout);
}

int main(void) {
    signal(SIGINT, sigint_handler);

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(fs_root, sizeof(fs_root), "%s/winterm_fs", home);

    init_fs();

    /* Start as Administrator in System32 */
    snprintf(win_cwd,  sizeof(win_cwd),  "C:\\Windows\\System32");
    snprintf(real_cwd, sizeof(real_cwd), "%s/Windows/System32", fs_root);
    chdir(real_cwd);

    /* Administrator environment */
    cmd_set("COMPUTERNAME=DESKTOP-WINTERM");
    cmd_set("OS=Windows_NT");
    cmd_set("PROCESSOR_ARCHITECTURE=AMD64");
    cmd_set("PROCESSOR_IDENTIFIER=AMD64 Family 23 Model 113 Stepping 0, AuthenticAMD");
    cmd_set("PROCESSOR_LEVEL=23");
    cmd_set("PROCESSOR_REVISION=7100");
    cmd_set("NUMBER_OF_PROCESSORS=12");
    cmd_set("SystemRoot=C:\\Windows");
    cmd_set("SystemDrive=C:");
    cmd_set("TEMP=C:\\Windows\\Temp");
    cmd_set("TMP=C:\\Windows\\Temp");
    cmd_set("USERNAME=Administrator");
    cmd_set("USERDOMAIN=DESKTOP-WINTERM");
    cmd_set("USERDOMAIN_ROAMINGPROFILE=DESKTOP-WINTERM");
    cmd_set("USERPROFILE=C:\\Users\\Administrator");
    cmd_set("HOMEDRIVE=C:");
    cmd_set("HOMEPATH=\\Users\\Administrator");
    cmd_set("APPDATA=C:\\Users\\Administrator\\AppData\\Roaming");
    cmd_set("LOCALAPPDATA=C:\\Users\\Administrator\\AppData\\Local");
    cmd_set("WINDIR=C:\\Windows");
    cmd_set("PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    cmd_set("PROMPT=$P$G");
    cmd_set("PATH=C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem;"
            "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\;"
            "C:\\Windows\\System32\\OpenSSH\\");
    cmd_set("PSModulePath=C:\\Program Files\\WindowsPowerShell\\Modules;"
            "C:\\Windows\\system32\\WindowsPowerShell\\v1.0\\Modules");
    cmd_set("PUBLIC=C:\\Users\\Public");
    cmd_set("ProgramFiles=C:\\Program Files");
    cmd_set("ProgramFiles(x86)=C:\\Program Files (x86)");
    cmd_set("ProgramData=C:\\ProgramData");
    cmd_set("CommonProgramFiles=C:\\Program Files\\Common Files");
    cmd_set("ALLUSERSPROFILE=C:\\ProgramData");
    cmd_set("SESSIONNAME=Console");

    printf("\r\nMicrosoft Windows [Version 10.0.19045.4291]\r\n"
           "(c) Microsoft Corporation. All rights reserved.\r\n\r\n");

    char input[MAX_INPUT];
    while (1) {
        printf("%s>", win_cwd);
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\r\n")] = 0;
        dispatch(input);
    }
    return 0;
}
