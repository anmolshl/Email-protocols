#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

static void handle_client(int fd);
void quitProcessPre(int fd);
void quitProcessPost(int fd, mail_list_t list);
void sendGreet(int fd);
int getArgStartIndex(char* line);

//Server states
static int isTransaction = 0;
static int isAuthorization = 0;
static int isUpdate = 0;

int main(int argc, char *argv[]) {

  if (argc != 2) {
    fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
    return 1;
  }

  run_server(argv[1], handle_client);

  return 0;
}

void handle_client(int fd) {
  isTransaction = 0;
  sendGreet(fd);//Sends opening message to client and enters authorization state

  //data
  char username[MAX_USERNAME_SIZE];
  char password[MAX_PASSWORD_SIZE];
  mail_list_t mail = NULL;

  //Local indicators
  int userNameEntered = 0;

  while(1) {
    net_buffer_t buffer = nb_create(fd, MAX_LINE_LENGTH);
    char line[MAX_LINE_LENGTH];
    int result = nb_read_line(buffer, line);
    if(result <= 0){
      //handle abrupt termination
      if(mail != NULL){
        reset_mail_list_deleted_flag(mail);
        destroy_mail_list(mail);
        break;
      }
      else{
        break;
      }
    }

    if(result > MAX_LINE_LENGTH){
      send_string(fd, "-ERR Command is too long\r\n");
      continue;
    }

    //Extract command and argument (if it exists)
    char command[MAX_LINE_LENGTH];
    strcpy(command, line);
    char args[MAX_LINE_LENGTH];
    if(strlen(command) < 6){
      send_string(fd, "-ERR Wrong command\r\n");
      continue;
    }
    int containsargs = 0;
    char* space = strchr(command, ' ');
    if(space == NULL) {
      command[strlen(command) - 2] = '\0';
    }
    else {
      if(strlen(command) > 7) {
        containsargs = 1;
        int startIndex = getArgStartIndex(line);
        strncpy(args, line+startIndex, MAX_LINE_LENGTH);
        args[strlen(args)-2] = '\0';
      }
      *space = '\0';
    }

    //Command comparison
    int isUSER = strcasecmp(command, "USER");
    int isPASS = strcasecmp(command, "PASS");
    int isQUIT = strcasecmp(command, "QUIT");
    int isSTAT = strcasecmp(command, "STAT");
    int isLIST = strcasecmp(command, "LIST");
    int isRETR = strcasecmp(command, "RETR");
    int isDELE = strcasecmp(command, "DELE");
    int isRSET = strcasecmp(command, "RSET");
    int isNOOP = strcasecmp(command, "NOOP");
    command[0] = '\0';
    line[0] = '\0';
    nb_destroy(buffer);

    //Command processing
    if (isQUIT == 0 && isTransaction == 0 && containsargs == 0) {
      quitProcessPre(fd);
      isAuthorization = 0;
      break;
    }

    if (isQUIT == 0 && isTransaction == 1 && containsargs == 0) {
      quitProcessPost(fd, mail);
      break;
    }

    if (isUSER == 0 && isAuthorization == 1 && containsargs == 1 && userNameEntered == 0) {
      strcpy(username, args);
      args[0] = '\0';

      //Check if username exists in users.txt
      if(is_valid_user(username, NULL) == 0) {
        send_string(fd, "-ERR No such user, enter again\r\n");
        username[0] = '\0';
        continue;
      }

      userNameEntered = 1;
      send_string(fd, "+OK User matched! Now enter password\r\n");
      continue;
    }

    if (isPASS == 0 && isAuthorization == 1 && userNameEntered == 1 && containsargs == 1) {
      strcpy(password, args);
      args[0] = '\0';

      //Check if password matches username
      if (!(is_valid_user(username, password)) == 0) {
        send_string(fd, "+OK Password matched! Enter desired command\r\n");
        mail = load_user_mail(username);
        isAuthorization = 0;    //exit autorization state
        isTransaction = 1;      //enter transaction state
      }
      else{
        userNameEntered = 0;    //reset username previously entered
        username[0] = '\0';
        password[0] = '\0';     //reset invalid password
        send_string(fd, "-ERR Invalid password, enter username and password again\r\n");
      }

      continue;
    }

    if (isSTAT == 0 && isTransaction == 1 && containsargs == 0) {
      int mailCount = get_mail_count(mail);
      char strMailCount[5];
      strMailCount[0] = '\0';
      sprintf(strMailCount, "%d", mailCount);
      int mailListSize = (int) get_mail_list_size(mail);
      char strMailListSize[5];
      strMailListSize[0] = '\0';
      sprintf(strMailListSize, "%d", mailListSize);
      char resp1[MAX_LINE_LENGTH];
      resp1[0] = '\0';
      strcat(resp1, "+OK ");
      strcat(resp1, strMailCount);
      strcat(resp1, " ");
      strcat(resp1, strMailListSize);
      strcat(resp1, "\r\n");
      send_string(fd, "%s", resp1);
      resp1[0] = '\0';
      continue;
    }

    if (isLIST == 0 && isTransaction == 1) {
      int mail_count = get_mail_count(mail);

      //Check if mail is available to list
      if (mail_count == 0) {
        send_string(fd, "+OK No mail .\r\n");
      }
      else {
        //Check if any argument was provided
        if (strlen(args) > 0) {
          unsigned int message_number = (unsigned int) atoi(args);
          if (message_number <= get_mail_count(mail)) {
            mail_item_t mail_item = get_mail_item(mail, message_number-1);

            //Selected mail was not found
            if (mail_item == NULL) {
              send_string(fd, "-ERR No such mail exists\r\n");
            }
            else {//found selected mail
              int size = (int) get_mail_item_size(mail_item);
              char sizestr[5];
              sizestr[0] = '\0';
              char respx[MAX_LINE_LENGTH];
              respx[0] = '\0';
              strcat(respx, "+OK ");
              strcat(respx, args);
              strcat(respx, " ");
              sprintf(sizestr, "%d", size);
              strcat(respx, sizestr);
              strcat(respx, "\r\n");
              send_string(fd, respx);
              args[0] = '\0';
              respx[0] = '\0';
            }
          }
          else {
            send_string(fd, "-ERR No such mail exists\r\n");
            args[0] = '\0';
          }
        }
        else {  //Case where no arguments are present
          unsigned int i = 0;
          char strMailCount[5];
          strMailCount[0] = '\0';
          sprintf(strMailCount, "%d", mail_count);
          size_t size_x = (size_t) mail_count*MAX_LINE_LENGTH;
          int mailListSize = (int) get_mail_list_size(mail);
          char strMailListSize[5];
          strMailListSize[0] = '\0';
          sprintf(strMailListSize, "%d", mailListSize);
          char *resp2 = malloc(size_x);
          resp2[0] = '\0';
          strcat(resp2, "+OK ");
          strcat(resp2, strMailCount);
          strcat(resp2, " messages (");
          strcat(resp2, strMailListSize);
          strcat(resp2, " octets)");
          strcat(resp2, "\r\n");
          int temp_i = 0;
          while (temp_i < mail_count) { //loop to get all valid (not deleted) email's info
            mail_item_t temp = get_mail_item(mail, i);
            if (temp != NULL) {
              int size = (int) get_mail_item_size(temp);
              char sizestr[5];
              sizestr[0] = '\0';
              sprintf(sizestr, "%d", size);
              char index[5];
              index[0] = '\0';
              sprintf(index, "%d", i+1);
              strcat(resp2, index);
              strcat(resp2, " ");
              strcat(resp2, sizestr);
              strcat(resp2, "\r\n");
              sizestr[0] = '\0';
              index[0] = '\0';
              temp_i++;
            }
            i++;
          }
          strcat(resp2, ".\r\n");
          send_string(fd, resp2);
          strMailCount[0] = '\0';
          strMailListSize[0] = '\0';
          free(resp2);
        }
      }
      continue;
    }

    if (isRETR == 0 && isTransaction == 1 && containsargs == 1) {
      unsigned int mail_del = (unsigned int) atoi(args);
      args[0] = '\0';
      mail_item_t temp = get_mail_item(mail, mail_del - 1);
      if (temp == NULL) {   //selected mail does not exist
        send_string(fd, "-ERR Mail does not exist\r\n");
      } else {
        char *resp3 = malloc(MAX_LINE_LENGTH+100);
        resp3[0] = '\0';
        int size = (int) get_mail_item_size(temp);
        char sizestr[5];
        sizestr[0] = '\0';
        sprintf(sizestr, "%d", size);
        strcat(resp3, "+OK ");
        strcat(resp3, sizestr);
        strcat(resp3, " octets");
        strcat(resp3, "\r\n");
        char* filename = malloc(FILENAME_MAX);
        filename[0] = '\0';
        strcpy(filename, get_mail_item_filename(temp));
        char* message = malloc(MAX_LINE_LENGTH);
        message[0] = '\0';
        //This part reads email file and moves the content to a local string
        FILE * file;
        file = fopen(filename, "r");
        fseek(file, 0, SEEK_END);
        long fsize = ftell(file);
        fseek(file, 0, SEEK_SET);
        fread(message, fsize, 1, file);
        fclose(file);
        //File read ends here
        message[fsize] = '\0';
        strcat(resp3, message);
        strcat(resp3, "\r\n");
        strcat(resp3, ".\r\n");
        send_string(fd, resp3);
        free(filename);
        free(message);
        free(resp3);
      }
      continue;
    }

    if (isDELE == 0 && isTransaction == 1 && containsargs == 1) {
      unsigned int mail_del = (unsigned int) atoi(args);    //Get index of mail to be deleted
      args[0] = '\0';
      mail_item_t temp = get_mail_item(mail, mail_del - 1);
      if (temp == NULL) {
        send_string(fd, "-ERR Mail does not exist\r\n");
      } else {
        mark_mail_item_deleted(temp);
        send_string(fd, "+OK Mail marked as deleted\r\n");
      }
      continue;
    }

    if (isRSET == 0 && isTransaction == 1 && containsargs == 0) {
      reset_mail_list_deleted_flag(mail);   //reset all delete flags in mail list
      send_string(fd, "+OK Successfully reset deleted mail\r\n");
      continue;
    }

    if (isNOOP == 0 && isTransaction == 1 && containsargs == 0) {
      send_string(fd, "+OK\r\n");
      continue;
    }

    send_string(fd, "-ERR Error, Check your command\r\n");
  }
}

///Helpers:

//Method sends initial POP greeting
void sendGreet(int fd){
  struct utsname uName;
  uname(&uName);
  char greeting[(int)sizeof(uName.nodename)+30];
  greeting[0] = '\0';
  strcat(greeting, "+OK POP3 Server Ready for ");
  strcat(greeting, uName.nodename);
  strcat(greeting, "! Now enter username");
  strcat(greeting, "\r\n");
  send_string(fd, "%s", greeting);
  isAuthorization = 1;
}

//Method processes QUIT post authorization
void quitProcessPost(int fd, mail_list_t list){
  char quitMessage[30];
  quitMessage[0] ='\0';
  strcat(quitMessage, "+OK POP3 Server quitting...\r\n");
  send_string(fd, "%s", quitMessage);
  isTransaction = 0;
  isUpdate = 1;
  destroy_mail_list(list);
  isUpdate = 0;
}

//Method processes QUIT pre authorization
void quitProcessPre(int fd){
  char quitMessage[30];
  quitMessage[0] ='\0';
  strcat(quitMessage, "+OK POP3 Server quitting...\r\n");
  send_string(fd, "%s", quitMessage);
}

//Gets starting index of argument
int getArgStartIndex(char* line){
  int index = 0;
  for(int i = 0; i < sizeof(line); i++){
    if(line[i] == ' '){
      index = i+1;
    }
  }
  return index;
}
