/* =============== Includes =============== */

/* Defining feature test macros to avoid potential
 * compiler warnings */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* =============== Defines =============== */

#define SPIKE_VERSION "0.0.1"
#define SPIKE_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

/* Uses large ints, as to avoid conflicts with other 
 * regular keypresses */
enum editorKey {
  ARROW_LEFT = 1000,    
  ARROW_RIGHT,          /* 1001 */ 
  ARROW_UP,             /* 1002 */
  ARROW_DOWN,           /* 1003, ... */
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/* =============== Data =============== */

/* Data type for storing a row of text in the editor. 
 * typedef allows us to refer to the type as erow 
 * instead of struct erow */
typedef struct erow {    
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

/* Stores the state of the editor */
struct editorConfig {
  int cx, cy;
  int rx;                         /* Index into the render field of an erow */
  int rowoff;                     /* row offset */
  int coloff;                     /* column offset */
  int screenrows;
  int screencols;
  int numrows;
  erow *row;                      /* Pointer to an array of erow structs */
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
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
     * Page up/down key, Del key, or Home/End key escape 
     * sequence. If it is, the corresponding key is returned */
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
	if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
	if (seq[2] == '~') {
	  switch (seq[1]) {
	    case '1': return HOME_KEY;
	    case '3': return DEL_KEY;
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

/* =============== Row Operations =============== */

/* Converts a chars index into a render index */
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (SPIKE_TAB_STOP - 1) - (rx % SPIKE_TAB_STOP);
    rx++;
  }
  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') tabs++;
  }
  
  free(row->render);

  /* The maximum number of characters needed for each tab 
   * is 8. tabs is multiplied by 7 because row->size already
   * counts 1 for each tab */
  row->render = malloc(row->size + tabs*(SPIKE_TAB_STOP - 1) + 1);

  int index = 0;

  /* Copies each character from chars to render. Tabs are 
   * rendered as multiple space characters */
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[index++] = ' ';
      while (index % SPIKE_TAB_STOP != 0) row->render[index++] = ' ';
    } else {
      row->render[index++] = row->chars[j];
    }
  }
  row->render[index] = '\0';
  row->rsize = index;
}

/* Initializes E.row */
void editorAppendRow(char *s, size_t len) {

  /* Reallocates a bigger block of memory according to the number  
   * of bytes each erow takes * the number of rows we want */
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;  
  E.row[at].size = len;

  /* Allocates a block of memory according to the size of the string */
  E.row[at].chars = malloc(len + 1);
  
  /* Copies the string into the memory that was allocated */
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);
  
  E.numrows++;
}

/* =============== File I/O =============== */

/* Allows the user to open a preexisting file */
void editorOpen(char *filename) {
  free(E.filename);

  /* Makes a copy of the given string, or the file's name in
   * this case, and allocates the required memory */
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;

  /* A data type that is used to represent the size of objects
   * in bytes */
  size_t linecap = 0;    /* Line capacity */    
  ssize_t linelen;       /* Signed version (can represent -1) */

  /* Passing in a null line pointer and a linecap of 0, so that
   * it allocates new memory for each line it reads. It sets line 
   * to point to the memory and linecap to the amount of memory it
   * allocated. Returns the length of the line read, or -1 if it is 
   * at the end of a file */
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
			   line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
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

/* Sets the values of E.rx, E.rowoff and E.coloff */
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  
  /* Checks if the cursor is above the visible window. If so, scrolls
   * up to where the cursor is */
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  /* Checks if the cursor is below the visible window. If so, scrolls
   * down to where the cursor is */
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

/* Writes out to the user's file */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      
      /* Displays a welcome message */
      if (E.numrows == 0 && y == E.screenrows / 3) {
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
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    
    abAppend(ab, "\x1b[K", 3);    /* Erases part of the current line */
    abAppend(ab, "\r\n", 2);
  }
}

/* Creates a status bar at the bottom of the program */
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);    /* Switches to inverted colors */
  char status[80], rstatus[80];

  /* Displays up to 20 characters of the filename and the
   * number of lines in the file */
  int len = snprintf(status, sizeof(status), "%.20s --- %d lines",
		     E.filename ? E.filename : "[No Name]", E.numrows);

  int curline = E.cy + 1;    /* current line */
  int curpercent;

  /* Done to avoid dividing by 0 when a new file is opened */
  if (E.numrows != 0) {
    curpercent = (curline * 100) / E.numrows;
  } else {
    curpercent = 0;
  }
  if (curpercent > 100) curpercent = 100;      
      
  /* Displays the current line number and percentage */
  int rlen = snprintf(rstatus, sizeof(rstatus), "L%d | %d%%",
			curline, curpercent);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);

  /* Ensures that the second status string is only printed if it
   * were to reach the right edge of the screen when printed */
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);     /* Switches back to regular formatting */
  abAppend(ab, "\r\n", 2);       /* Prints a new line after the status bar */
}

/* Creates a message bar at the very bottom of the program */
void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);           /* Clears the message bar */
  int msglen = strlen(E.statusmsg);

  /* Shortens the statusmsg if it is longer than the width 
   * of the screen */
  if (msglen > E.screencols) msglen = E.screencols;

  /* If there is a message and it is less than 5 seconds
   * old, display the message */
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

/* Sets up the editing environment */
void editorRefreshScreen() {
  editorScroll();
  
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);    /* Hides the cursor */
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  
  char buf[32];
  snprintf(buf, sizeof(buf), "x1b[%d;%dH", (E.cy - E.rowoff) + 1,
	                                   (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));
  
  abAppend(&ab, "\x1b[25h", 6);     /* Shows the cursor */

  /* Writes the buffer's content out to standard output */
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* The ... argument indicates that this is a variadic function, 
 * which means that it can take any number of arguments. The 
 * functions, va_start() and va_end(), have to be called on
 * a value of type va_list*/
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;

  /* The last argument before ... must be passed, so that the 
   * address of the next argument(s) is known */
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);

  /* Sets E.statusmsg_time to the current time */
  E.statusmsg_time = time(NULL);
}

/* =============== Input =============== */

/* Moves the cursor based on the user's input */ 
void editorMoveCursor(int key) {

  /* Checks if the cursor is on an actual line. If it is, the
   * row variable will point to the erow that the cursor is on */
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
	E.cx--;
      } else if (E.cy > 0) {            /* Allows the user to move left at the */
	E.cy--;                         /* start of a line */
	E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
	E.cx++;
      } else if (row && E.cx == row->size) {  /* Allows the user to move right */
	E.cy++;                               /* at the end of a line */
	E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
	E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
	E.cy++;
      }
      break;
  }

  /* Setting row again because E.cy could point to a different
   * line now */
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  /* A NULL line (new line) has a size of 0 */
  int rowlen = row ? row->size : 0;        

  /* Sets E.cx to the end of the line if the cursor is past the 
   * end of the line */
  if (E.cx > rowlen) {
    E.cx = rowlen;
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
      if (E.cy < E.numrows)
	E.cx = E.row[E.cy].size;
      break;
      
    /* Simulates Page up/down by inputting ARROW_UP/DOWN many times */
    case PAGE_UP:
    case PAGE_DOWN:
      {
	if (c == PAGE_UP) {
	  E.cy = E.rowoff;
	} else if (c == PAGE_DOWN) {
	  E.cy = E.rowoff + E.screenrows - 1;
	  if (E.cy > E.numrows) E.cy = E.numrows;
	}
	
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
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;        /* Will stay NULL if a file is not opened */
  E.statusmsg[0] = '\0';    /* No message will be displayed by default */
  E.statusmsg_time = 0;
  
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");

  /* Gets decremented so that editorDrawRows() does not
   * draw lines of text at the bottom of the screen */
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-Q = quit");
  
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  
  return 0;
}
