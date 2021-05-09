#include <stdio.h>
#include <errno.h>
// non standard realization - support for virtually change ONLY ONE char

int ungetc(int c, FILE* file)
{
	int res;

    if(!file){
        errno = EBADF;
        return EOF;
    }

	if (file->mode != _STDIO_F_R){
        errno = EACCES;
        return EOF;
    }

	if (file->position > file->start_size || file->position == 0 || c == EOF || file->__ungetc_emu_buff != EOF)
	{
	    errno = EOF;
		return EOF;
	}
	
	file->__ungetc_emu_buff = c;
	file->position--;
	return c;
}
