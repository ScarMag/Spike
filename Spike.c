// Step 30
/* =============== Includes =============== */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* =============== Defines =============== */

#define CTRL_KEY(k) ((k) & 0x1f)

/* =============== Data =============== */

/* Stores the state of the editor */
struct editorConfig {
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

/* =============== Terminal =============== */

/* Error handling */
void die(const char *s) {

  /* Clears the screen */ 
  write(STDOUT_FILENO, "\x1b[2J", 4);        /* "\x1b" is an escape character */

  /* Repositions the cursor */
  write(STDOUT_FILENO, "\x1b[H", 3);         /* Creates an escape sequence when combined 
					      * with other bytes (VT100 escape sequence) */
  perror(s);
  exit(1);
}

/* Restores terminal's original attributes */
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

/* Disables unwanted features/keypresses (echo, Ctrl-'x') */ 
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);
  
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
}

// Queries the terminal for the position of the cursor */
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  
  /* "n" queries the terminal for status information while 
   * "6" asks for the cursor position */
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }

  /* Appends '\0' to the end of the string to determine where 
   * it terminates when using printf() */
  buf[i] = '\0';

  /* Makes sure that we are dealing with an escape sequence */
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;

  /* Parses a string of the form %d;%d, which contains two
   * integers separated by a semicolon. The parsed values 
   * are then put into the rows and cols variables */
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

/* Sets the parameters to the height and width of the terminal window */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  /* I/O Control function that sets ws to the value returned by 
   * TIOCGWINSZ (Terminal I/O Control Get Window Size???) */
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/* =============== Append Buffer =============== */

/* Creating a dynamic string type that only supports appending */
struct abuf {
  char *b;
  int len;
};

/* Constructor for the abuf type */ 
#define ABUF_INIT {NULL, 0}

/* Appends a string to an abuf (our dynamic string type) */
void abAppend(struct abuf *ab, const char *s, int len) {

  /* Allocates a block of memory that is the size of the current 
   * string + the size of the new string being appended */
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;

  /* Copies the new string, s, after the end of the current string */
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/* Destructor that deallocates the dynamic memory used by an abuf */
void abFree(struct abuf *ab) {
  free(ab->b);
}

/* =============== Output =============== */

void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "-_-", 3);

    if (y < E.screenrows - 1) {
      write(STDOUT_FILENO, "\r\n", 2);
    }
  }
}

/* Sets up the editing environment */
void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();

  write(STDOUT_FILENO, "\x1b[H", 3);
}

/* =============== Input =============== */

void editorProcessKeypress() {
  char c = editorReadKey();
  
  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

/* =============== Init =============== */

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();
  
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  
  return 0;
}
