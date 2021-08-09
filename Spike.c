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

#define SPIKE_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

/* Uses large ints, as to avoid conflicts with other 
 * regular keypresses */
enum editorKey {
  ARROW_LEFT = 1000,    
  ARROW_RIGHT,          /* 1001 */ 
  ARROW_UP,             /* 1002 */
  ARROW_DOWN,            /* 1003, ... */
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/* =============== Data =============== */

/* Stores the state of the editor */
struct editorConfig {
  int cx, cy;
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

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    /* Reads 2 more bytes into the seq buffer to determine if
     * it is an escape sequence or if the user just pressed the 
     * Escape key */
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    /* Determines if the escape sequence is an arrow key,   
     * Page up/down key, or Home/End key escape sequence. If it 
     * is, the corresponding key is returned */
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
	if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
	if (seq[2] == '~') {
	  switch (seq[1]) {
	    case '1': return HOME_KEY;
	    case '4': return END_KEY;
	    case '5': return PAGE_UP;
	    case '6': return PAGE_DOWN;
	    case '7': return HOME_KEY;     /* Actual sequence is dependent on */     
	    case '8': return END_KEY;      /* user's OS or terminal emulator */      
	  }
	}
      } else {
	switch (seq[1]) {
	  case 'A': return ARROW_UP;
	  case 'B': return ARROW_DOWN;
	  case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
	  case 'H': return HOME_KEY;
	  case 'F': return END_KEY;
	}
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    
    return '\x1b';
  } else {
    return c;
  }
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

/* Appends a string to an abuf type (our dynamic string type) */
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

/* Destructor that deallocates the dynamic memory used by an abuf type*/
void abFree(struct abuf *ab) {
  free(ab->b);
}

/* =============== Output =============== */

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {

    /* Displays a welcome message */
    if (y == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
          "Spike editor -- version %s", SPIKE_VERSION);

      /* Truncates the length of the string to make sure it fits 
       * in the terminal */
      if (welcomelen > E.screencols) welcomelen = E.screencols;

      /* Centers the welcome message by dividing the screen's width
       * by 2, and then subtracting half of the message's length
       * from that */
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
	abAppend(ab, "-_-", 3);
	padding--;
      }
      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } else {
      abAppend(ab, "-_-", 3);
    }

    abAppend(ab, "\x1b[K", 3);    /* Erases part of the current line */
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/* Sets up the editing environment */
void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);    /* Hides the cursor */
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));
  
  abAppend(&ab, "\x1b[25h", 6);     /* Shows the cursor */

  /* Writes the buffer's content out to standard output */
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* =============== Input =============== */

void editorMoveCursor(int key) {
  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
	E.cx--;
      }
      break;
    case ARROW_RIGHT:
      if (E.cx != E.screencols - 1) {
	E.cx++;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
	E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy != E.screenrows - 1) {
	E.cy++;
      }
      break;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();
  
  switch (c) {
    case CTRL_KEY('q'):                      /* Exits the editor program */ 
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      E.cx = E.screencols - 1;
      break;
      
    /* Simulates Page up/down by inputting ARROW_UP/DOWN many times */
    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screenrows;

	/* ?: as a ternary operator -> "if a then b, otherwise c" */
        while (times--)
	  editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
      
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/* =============== Init =============== */

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  
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
