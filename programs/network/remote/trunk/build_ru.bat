@erase lang.inc
@echo lang fix ru >lang.inc
@fasm remote.asm remote
@erase lang.inc
@pause