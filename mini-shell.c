/* Mini-shell
 * Samoila Lavinia Andreea
 * 333CA
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "parser.h"

#ifdef UNICODE
#  error "Unicode not supported in this source file!"
#endif

#define PROMPT_STRING	"> "

typedef struct my_string{
	char *str;		/* Sir de caractere - comanda, sau parametru al comenzii. */
	unsigned int dim;	/* Cat spatiu a fost alocat pentru sirul respectiv. */
} * MyString;

#define DEBUG			0

#define INIT_STRING_ALLOC	50	/* Spatiul initial alocat pentru un sir. */
#define INIT_NR_PARAMS		10	/* Nr initial de parametrii pt o comanda. */

#define OVERWRITE		0	/* Daca o variabila de mediu va fi rescrisa sau nu. */
#define NO_OVERWRITE		1

#define EXIT_ERROR		1

#define OUT_APPEND		1	/* Definitii folosite cand deschid fisierele pt redirectari. */
#define ERR_APPEND		2
#define NO_APPEND		0
#define READ			-1
#define IMPLICIT_RIGHTS		0644

#define PIPE_LEFT		0
#define PIPE_RIGHT		1
#define PARALLEL		2

#define STDIN			0
#define STDOUT			1
#define STDERR			2
	

int executeTree(command_t *);

void parse_error(const char * str, const int where)
{
	fprintf(stderr, "Parse error near %d: %s\n", where, str);
}

/* Seteaza valoarea _value_ variabilei _var_. */
void setVar(const char *var, const char *value){
	int returnVal;	
	returnVal = setenv(var, value, NO_OVERWRITE);
	if(returnVal != 0){
		fprintf(stderr, "Could not set var %s to %s\n", var, value);
	}
}

/* Daca variabila este setata, ii intoarce valoarea. */
const char * expandVar(word_t * word){
	if(word->expand)
		return getenv(word->string);
	return word->string;
}

/* Intoarce o structura de tipul MyString.
 * Structura contine un sir si lungimea maxima a acestuia
 * (cata memorie a fost alocata pentru el).
 */
MyString allocMyString(int init){
	MyString s = (MyString) calloc(1, sizeof(struct my_string));
	s->str = (char *) calloc(init, sizeof(char));
	memset(s->str, 0, init);
	s->dim = init;
	return s;
}


/* Concateneaza doua siruri realocandu-l pe primul daca este necesar. */
MyString myString_cat_alloc(MyString first, const char *second){
	if(second == NULL)	
		return first;
	while((strlen(first->str) + strlen(second)) > first->dim){
		first->dim *= 2;
		first->str = realloc(first->str, first->dim);	
	}
	first->str = strcat(first->str, second);
	return first;
}

/* Intoarce un cuvant, expandand ce variabile contine. */
char * getWord(word_t * w){
	if(w == NULL) return NULL;
	word_t *crt = w;
	/* Expresie de forma x=y. */
	if(crt->next_part != NULL && strcmp(crt->next_part->string, "=") == 0){
		setVar(expandVar(crt), 
			expandVar(crt->next_part->next_part));
		return NULL;
	}
	
	/* Uneste partile unui cuvand, expandand unde este cazul. */
	MyString word = allocMyString(INIT_STRING_ALLOC);
	while(crt != NULL){
		word = myString_cat_alloc(word, expandVar(crt));
		crt = crt->next_part;
	}
	char *s = word->str;
	if(s == NULL)
		free(word->str);
	free(word);
	return s;
}

/* Intoarce o lista cu parametrii. */
char **getParams(word_t *w, char *cmd){
	char **param;
	char * tmp;
	int size = INIT_NR_PARAMS, nr = 1;
	param = (char **)calloc(size, sizeof(char *));
	/* Primul parametru este chiar comanda. */
	param[0] = cmd;
	word_t *crt = w;
	while(crt != NULL){
		tmp = getWord(crt);	
		if(tmp != NULL){
			param[nr] = tmp;
			nr++;
		}
		if(nr == size - 1){
			size *= 2;
			param = (char**)realloc(param, size);
		}
		crt = crt->next_word;
	}
	/* Lista trebuie sa se termine cu (char*)NULL. */
	param[nr] = (char*)NULL;
	return param;
}

/* Elibereaza memoria alocata pentru parametrii. */
void freeParams(char **params){
	int i = 0;
	if(params){
		while(params[i] != NULL){
			free(params[i]);
			i++;
		}
	} else {
		if(DEBUG) fprintf(stderr, "Params already freed\n");
	}
	free(params);
}

/* Redirectioneaza: ce trebuia scris in _from_ va fi trimis in _filename_. */
int do_redirect(int from, char * filename, int mode){
	int fd, returnVal;
	
	returnVal = close(from);
	if(returnVal != 0){
		fprintf(stderr, "Could not redirect.\n");
		return -1;
	}

	switch(mode){
	case OUT_APPEND: case ERR_APPEND:
		fd = open(filename, O_CREAT | O_APPEND | O_RDWR, IMPLICIT_RIGHTS);
		break;
	case NO_APPEND:
		fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, IMPLICIT_RIGHTS);
		break;
	default:
		fd = open(filename, O_RDWR);
	}

	if(fd == -1)
		printf("Could not open file %s\n", filename);
	return fd;	
}

/* Executa o comanda simpla, facand ce redirectionari sunt necesare. */
void executeSimpleCommand(simple_command_t * scmd, char *cmd, char **params){
	int fds[3] = {-1};
	char * out = getWord(scmd->out);
	char * in = getWord(scmd->in);
	char * err = getWord(scmd->err);

	/* Redirectioneaza stdout si stderr. */
	if(out != NULL && err != NULL && strcmp(out, err) == 0){
		fds[1] = do_redirect(STDOUT, out, scmd->io_flags & IO_OUT_APPEND);
		fds[2] = dup2(STDOUT, STDERR);
	} else {
		if(scmd->out != NULL)
			fds[1] = do_redirect(STDOUT, out, scmd->io_flags & IO_OUT_APPEND);
		if(scmd->err != NULL)
			fds[2] = do_redirect(STDERR, err, scmd->io_flags & IO_ERR_APPEND);
	}

	/* Redirectioneaza stdin. */
	if(scmd->in != NULL)
		fds[0] = do_redirect(STDIN, in, READ);

	/* Elibereaza memoria alocata numelor de fisiere. */
	free(in); free(out); free(err);	

	/* Executa comanda. */
	if(cmd != NULL){
		int err = 0;
		free_parse_memory();
		err = execvp(cmd, params);
		if(err == -1)
			fprintf(stderr, "Execution failed for '%s'\n", cmd);
		free_parse_memory();
		exit(EXIT_FAILURE);
	}
			
}


/* Executa o comanda interna (fara a face fork) 
 * sau o comanda simpla (face fork).
 */
int executeCommand(command_t *root, char *cmd, char **params){
		int status;
		/* Comenzi interne. */
		if(cmd != NULL && strcmp(cmd, "cd") == 0){
			int returnVal =	chdir(params[1]);
			if(returnVal)
				fprintf(stderr, "Could not change current directory to %s\n", params[1]);
			return 0;
		}
		if(cmd != NULL && (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)){
			freeParams(params);
			free_parse_memory();
			exit(0);
		}

		/* Comanda simpla. */
		switch(fork()){
			case -1:
				fprintf(stderr, "Could not fork\n"); 
				break;
			case 0:
				executeSimpleCommand(root->scmd, cmd, params);
				freeParams(params);
				break;
			default:
				wait(&status);
				return status;
		}
		return 0;
}

/* Creeaza un proces nou, si continua executia arborelui.
 * Daca se apeleaza din executia in paralel, nu face nimic
 * in plus. Daca se apeleaza un urma unui pipe, redirecteaza
 * intrarea si iesirea in functie de tipul procesului
 */
void forkProcess(command_t *cmd, int type, int *pfd){

	switch(fork()){
		case -1:
			fprintf(stderr, "Error in fork\n");
			break;
		case 0:
			if(type == PIPE_LEFT) {
				close(pfd[0]);
				close(STDOUT);
				dup(pfd[1]);
				close(pfd[1]);
			} else 
			if(type == PIPE_RIGHT){
				close(pfd[1]);
				close(STDIN);
				dup(pfd[0]);
				close(pfd[0]);
			}

			executeTree(cmd);
			free_parse_memory();
			exit(0);
		default:
			break;
	}
}

/* Parcurge arborele intors de parser si executa comenzile. */
int executeTree(command_t * root){
	int retVal = 0;
	if(root->op == OP_NONE){
		assert(root->cmd1 == NULL);
		assert(root->cmd2 == NULL);
		char *cmd = getWord(root->scmd->verb); 
		char **params = getParams(root->scmd->params, cmd);
		if(cmd != NULL){ 
			retVal = executeCommand(root, cmd, params);
			freeParams(params);
		}
		return retVal;
	} else {
		int pfd[2];
		assert(root->scmd == NULL);
		switch(root->op){
			case OP_SEQUENTIAL:
				retVal = executeTree(root->cmd1);
				retVal = executeTree(root->cmd2);
				break;
			case OP_CONDITIONAL_ZERO:
				if(executeTree(root->cmd1) == 0)
					retVal = executeTree(root->cmd2);
				break;
			case OP_CONDITIONAL_NZERO:
				if((retVal = executeTree(root->cmd1)))
					retVal = executeTree(root->cmd2);
				break;	
			case OP_PIPE:
				pipe(pfd);
				
				forkProcess(root->cmd2, PIPE_RIGHT, pfd);
				forkProcess(root->cmd1, PIPE_LEFT, pfd);
				
				close(pfd[0]); close(pfd[1]);
				wait(&retVal); wait(&retVal);
				break;
			case OP_PARALLEL:
				forkProcess(root->cmd1, PARALLEL, pfd);
				forkProcess(root->cmd2, PARALLEL, pfd);			

				wait(&retVal); wait(&retVal);
				break;	
			default:
				break;			
		}
	}	
	return retVal;
}

int main(void)
{
	char line[1000];
	int status;
	while(1){
		status = 0;
		command_t *root = NULL;
		printf(PROMPT_STRING); fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL)
		{
			fprintf(stderr, "End of file!\n");
			return EXIT_SUCCESS;
		}
		if (parse_line(line, &root)) {
			if(DEBUG) printf("Command successfully read!\n");
			if (root == NULL) {
				printf("Command is empty!\n");
			}
			else {
				executeTree(root);
			}
		}
		else {
			/* there was an error parsing the command */
			parse_error("", 0);
		}

		free_parse_memory();
	}
	return EXIT_SUCCESS;
}
