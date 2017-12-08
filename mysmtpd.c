#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

struct user_list {
  char *user;
  struct user_list *next;
};

static void handle_client(int fd);
void send_message(int fd, char* code, char* message, int size);
int receive_helo(int fd);
void handle_mail(int fd);
int check_address(int fd, char* rest);
int save_file(int fd, user_list_t rcpts);  

int main(int argc, char *argv[]) {
  
  if (argc != 2) {
    fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
    return 1;
  }
  
  run_server(argv[1], handle_client);
  
  return 0;
}

void handle_client(int fd) {

  send_message(fd, "220", "service ready", 20);
  int quit = receive_helo(fd);
  if (quit == 1) {
  	return;
  }
  handle_mail(fd);
}

// sends a message to the client
// includes the domain name of the server in the message
// Parameters:
//    fd: socket file descriptor
//    code: a string representing the code to send
//    message: a string representing the rest of the message
//    size: upper bound on size of the message in bytes
void send_message(int fd, char* code, char* message, int size) {
  struct utsname uName;
  uname(&uName);
  char* msg = malloc(sizeof(uName.nodename) + size);
  strcpy(msg, code);
  strcat(msg, " ");
  strcat(msg, uName.nodename);
  strcat(msg, " ");
  strcat(msg, message);
  strcat(msg, "\r\n");
  send_string(fd, msg);
  free(msg);
}

// receives the initial HELO message
// also handles NOOP and QUIT
// Returns 0 if HELO was sent, 1 if QUIT was sent
int receive_helo(int fd) {
  char buf[MAX_LINE_LENGTH];	
  net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);
  int result = nb_read_line(nb, buf);
  printf("%s\n", buf);

  // connection was closed
  if (result <= 0) {
  	return 1;
  }

  // line too long
  if (result > MAX_LINE_LENGTH) {
  	send_string(fd, "552 line exceeded max size\r\n");
  	return receive_helo(fd);
  }

  // grab the command by cutting off at spacebar
  char code[MAX_LINE_LENGTH];
  strcpy(code, buf);
  int empty = 0;
  char* space = strchr(code, ' ');
  char* rest = NULL;

  // command is under 4 letters long
  if (strlen(code) < 6) {
  	send_string(fd, "500 command not recognized\r\n");
    return receive_helo(fd);
  }

  if(space == NULL) {
  	// no spaces, strip the CRLF
  	code[strlen(code) - 2] = '\0';
  } else {
  	// is there text after the space?
  	if(strlen(code) > 7) {
  		empty = 1;
  		rest = code + 5;
  	}
  	*space = '\0';
  }

  int is_helo = strcasecmp(code, "HELO");
  int is_ehlo = strcasecmp(code, "EHLO");
  int is_noop = strcasecmp(code, "NOOP");
  int is_quit = strcasecmp(code, "QUIT");
  int is_mail = strcasecmp(code, "MAIL");
  int is_rcpt = strcasecmp(code, "RCPT");
  int is_rset = strcasecmp(code, "RSET");
  int is_vrfy = strcasecmp(code, "VRFY");
  int is_expn = strcasecmp(code, "EXPN");
  int is_help = strcasecmp(code, "HELP");
  int is_data = strcasecmp(code, "DATA");
  nb_destroy(nb);

  // unimplemented commands
  if((is_ehlo == 0) || (is_rset == 0) || (is_vrfy == 0) || (is_expn == 0) || (is_help == 0)) {
  	send_string(fd, "502 command not implemented\r\n");
  	return receive_helo(fd);
  }

  // out of order commands
  if((is_mail == 0) || (is_rcpt == 0) || (is_data == 0)) {
  	send_string(fd, "503 please send HELO before engaging in mail transactions\r\n");
  	return receive_helo(fd);
  }

  if(is_helo == 0) {
  	if (empty == 0) {
  	  send_string(fd, "501 HELO requires name\r\n");
  	  return receive_helo(fd);
  	} else {
  	  // truncate leading space and CRLF
  	  while(*rest == ' ') {
  	  	rest = rest + 1;
  	  }
  	  rest[strlen(rest) - 2] = '\0';
  	  // only spaces?
  	  if (strlen(rest) == 0) {
  	  	send_string(fd, "501 HELO requires name\r\n");
  	  	return receive_helo(fd);
  	  }
  	  // send HELO response
  	  char* msg = malloc(strlen(rest) + 7);
  	  strcpy(msg, "Hello ");
  	  strcat(msg, rest);
  	  send_message(fd, "250", msg, strlen(msg) + 5);
  	  free(msg);
  	  return 0;
  	}
  }

  if(is_noop == 0) {
  	send_message(fd, "250", "OK", 10);
  	return receive_helo(fd);
  }

  if(is_quit == 0) {
  	if (empty == 1) {
  		send_string(fd, "501 no parameters accepted for QUIT\r\n");
  		return receive_helo(fd);
  	}
  	send_message(fd, "221", "closing connection", 30);
  	return 1;
  }

  send_string(fd, "500 command not recognized\r\n");
  return receive_helo(fd);
}

// handles all server processing after HELO is completed
// Parameters:
//    fd: socket file descriptor
void handle_mail(int fd) {
  // state and other variables
  int is_mail_state = 1;
  int is_rcpt_state = 0;
  int is_data_state = 0;
  user_list_t rcpts = create_user_list();

  while(1) {
    char buf[MAX_LINE_LENGTH];	
    net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);
    int result = nb_read_line(nb, buf);
    printf("%s\n", buf);
    
    // connection was closed
    if (result <= 0) {
      nb_destroy(nb);
  	  break;
    }

    // line too long
    if (result > MAX_LINE_LENGTH) {
    	send_string(fd, "552 line exceeded max size\r\n");
    	continue;
    }

    char code[MAX_LINE_LENGTH];
    strcpy(code, buf);
    // text entered after the command
    char* rest = NULL;
    // was anything entered after the command?
    int empty = 0;
    nb_destroy(nb);

    // command is under 4 letters long
    if (strlen(code) < 6) {
  	  send_string(fd, "500 command not recognized\r\n");
      continue;
    }

    // grab the command by cutting off at first whitespace
    char* space = strchr(code, ' ');
    if(space == NULL) {
  	  // no spaces, strip the CRLF
  	  code[strlen(code) - 2] = '\0';
    } else {
  	  // is there text after the space and \n?
  	  if(strlen(code) > 7) {
  	  	empty = 1;
  	  	rest = code + 5;
  	  }
  	  *space = '\0';
    }

    int is_helo = strcasecmp(code, "HELO");
    int is_ehlo = strcasecmp(code, "EHLO");
    int is_noop = strcasecmp(code, "NOOP");
    int is_quit = strcasecmp(code, "QUIT");
    int is_mail = strcasecmp(code, "MAIL");
    int is_rcpt = strcasecmp(code, "RCPT");
    int is_rset = strcasecmp(code, "RSET");
    int is_vrfy = strcasecmp(code, "VRFY");
    int is_expn = strcasecmp(code, "EXPN");
    int is_help = strcasecmp(code, "HELP");
    int is_data = strcasecmp(code, "DATA");

    // unimplemented commands
    if((is_ehlo == 0) || (is_rset == 0) || (is_vrfy == 0) || (is_expn == 0) || (is_help == 0)) {
  	  send_string(fd, "502 command not implemented\r\n");
  	  continue;
    }

    // out of order commands
    if((is_helo == 0) || 
      ((is_rcpt == 0) && (is_rcpt_state != 1)) ||
      ((is_mail == 0) && (is_mail_state != 1)) ||
      ((is_data == 0) && (is_data_state != 1))) {

      send_string(fd, "503 command out of order\r\n");
      continue;
    }

    // NOOP
    if(is_noop == 0) {
      send_message(fd, "250", "OK", 10);
      continue;
    }

    // QUIT
    if(is_quit == 0) {
      if(empty == 1) {
      	send_string(fd, "501 parameters not accepted for QUIT\r\n");
      	continue;
      }
      send_message(fd, "221", "closing connection", 30);
      break;
    }

    // MAIL handling
    if(is_mail == 0) {
      // was there anything after the MAIL?
      if (empty == 0) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

      // is there enough for MAIL FROM:?
      int rest_len = strlen(rest);
      if (rest_len < 8) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

      // did the user actually send MAIL FROM:?
      char from[6];
      memcpy(from, rest, 5);
      from[5] = '\0';
      int is_from = strcasecmp(from, "FROM:");
      if (is_from != 0) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

      // truncate leading spaces
      rest = rest + 5;
      while(*rest == ' ') {
   	    rest = rest + 1;
      }

	  int valid = check_address(fd, rest);
	  if (valid == 1) {
	  	// invalid
        continue;
	  }
      // update state
      is_mail_state = 0;
      is_rcpt_state = 1;
      send_message(fd, "250", "Sender OK", 20);
      continue;  
    }

    // RCPT handling
    if(is_rcpt == 0) {
      // was there anything after the RCPT?
      if (empty == 0) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

      // did the client send enough for RCPT TO:?
      int rest_len = strlen(rest);
      if (rest_len < 6) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

      // did the user actually send RCPT TO:?
      char to[4];
      memcpy(to, rest, 3);
      to[3] = '\0';
      int is_to = strcasecmp(to, "TO:");
      if (is_to != 0) {
      	send_string(fd, "501 Syntax error\r\n");
      	continue;
      }

	  // truncate leading spaces
      rest = rest + 3;
      while(*rest == ' ') {
      	rest = rest + 1;
      }

      int valid = check_address(fd, rest);
      if (valid == 1) {
      	// invalid address
      	continue;
      }
      // trim the leading <
      rest = rest + 1;
      if (is_valid_user(rest, NULL) == 0) {
      	send_string(fd, "550 mailbox not accepted\r\n");
      	continue;
      }
      // update state
      add_user_to_list(&rcpts, rest);
      is_data_state = 1;
      send_message(fd, "250", "RCPT OK", 20);
      continue;
    }

    // DATA handling
    if(is_data == 0) {
      if (empty == 1) {
      	send_string(fd, "501 parameters not accepted for DATA\r\n");
      	continue;
      }
      send_string(fd, "354 accepting data, end with <CRLF>.<CRLF>\r\n");
      if (save_file(fd, rcpts) != 0) {
      	continue;
      }
      return handle_mail(fd);
    }

    send_string(fd, "500 command not recognized\r\n");
  }

  destroy_user_list(rcpts);
  
}

// validates the email address
// truncates the ending brace (>)
// Parameters:
//    fd: socket file descriptor
//    rest: pointer to email address
// Returns 1 if email is formatted badly
// Returns 0 if it is well-formed
int check_address(int fd, char* rest) {
  // check for spaces, braces, etc
  if (strlen(rest) < 5) {
  	send_string(fd, "501 Syntax error\r\n");
   	return 1;
  }

  char* has_langle = strchr(rest, '<');
  char* has_rangle = strchr(rest, '>');
  if ((has_langle == 0) || (has_rangle == 0)) {
  	send_string(fd, "501 Syntax error in address\r\n");
   	return 1;
  }
  char* has_param = strchr(has_rangle, ' ');
  if (has_param != 0) {
  	// did the client add parameters after the email address?
  	while(*has_param == ' ') {
  	  has_param = has_param + 1;
  	}
  	if (strlen(has_param) > 2) {
  	  send_string(fd, "555 mail parameters not implemented\r\n");
  	  return 1;
  	}
  }

  // make sure the mail starts with < and ends with >
  if ((*rest == '<') && (*(rest + strlen(rest) - 3) == '>')) {
  	rest = rest + 1;
  	rest[strlen(rest) - 3] = '\0';
  	// make sure there are no additional <>
  	has_langle = strchr(rest, '<');
  	has_rangle = strchr(rest, '>');
  	if ((has_rangle != 0) && (has_langle != 0)) {
  	  send_string(fd, "501 Syntax error in address\r\n");
      return 1;
   	}
   	return 0;
  }
  send_string(fd, "501 Syntax error in address\r\n");	
  return 1;
}

// receives and saves the email message
// returns 0 on success, 1 if an error occurred
// Parameters:
//    fd: socket file descriptor
//    rcpts: user_list_t with all recipients
int save_file(int fd, user_list_t rcpts) {
  // get a temporary file
  char template[] = "fileXXXXXX";
  int file_fd = mkstemp(template);
  char buf[MAX_LINE_LENGTH];
  net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);

  while(1) {
    int result = nb_read_line(nb, buf);
    printf("%s\n", buf);
    printf("%d\n", result);
    // connection was closed
    if (result <= 0) {
      nb_destroy(nb);
  	  return 1;
    }

    // line too long
    if (result > MAX_LINE_LENGTH) {
    	nb_destroy(nb);
    	send_string(fd, "552 line exceeded max size\r\n");
    	return 1;
    }

    // no need to write the last line
    if (strcmp(buf, ".\r\n") == 0) {
      break;
    }
    write(file_fd, buf, result);
  }

  nb_destroy(nb);
  save_user_mail(template, rcpts);
  remove(template);

  send_message(fd, "250", "message successfully sent", 35);
  destroy_user_list(rcpts);
  return 0;
}
