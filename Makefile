# Makefile para la practica "Procesos, llamadas al sistema e IPC en Linux"

GRUPO ?= Los_illuminati

CC      := gcc
CFLAGS  := -Wall -Wextra -g -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -DNOMBRE_GRUPO=\"$(GRUPO)\"
LDFLAGS := -pthread -lrt -lm

# Directorios del proyecto
DIR_COMUN := comun
DIR_FIFO  := fifo
DIR_SHM   := shm
DIR_BIN   := bin
DIR_OBJ   := obj

# Fuentes comunes y objetos
OBJS_COMUN_BASE := $(DIR_OBJ)/utilidades.o $(DIR_OBJ)/csv.o
OBJS_CLI        := $(DIR_OBJ)/interfaz.o
OBJS_SEM        := $(DIR_OBJ)/semaforos.o

# Ejecutables objetivo
OBJETIVOS := $(DIR_BIN)/servidor_fifo \
             $(DIR_BIN)/cliente_fifo  \
             $(DIR_BIN)/servidor_shm  \
             $(DIR_BIN)/cliente_shm

.PHONY: all clean limpiar-ipc directorios ayuda

all: directorios $(OBJETIVOS)

directorios:
	@mkdir -p $(DIR_BIN) $(DIR_OBJ)

# Modulos comunes
$(DIR_OBJ)/utilidades.o: $(DIR_COMUN)/utilidades.c $(DIR_COMUN)/utilidades.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_OBJ)/csv.o: $(DIR_COMUN)/csv.c $(DIR_COMUN)/csv.h $(DIR_COMUN)/protocolo.h $(DIR_COMUN)/utilidades.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_OBJ)/interfaz.o: $(DIR_COMUN)/interfaz.c $(DIR_COMUN)/interfaz.h $(DIR_COMUN)/protocolo.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_OBJ)/semaforos.o: $(DIR_COMUN)/semaforos.c $(DIR_COMUN)/semaforos.h $(DIR_COMUN)/utilidades.h
	$(CC) $(CFLAGS) -c $< -o $@

# Parte A: FIFO
$(DIR_OBJ)/servidor_fifo.o: $(DIR_FIFO)/servidor_fifo.c $(DIR_COMUN)/protocolo.h $(DIR_COMUN)/utilidades.h $(DIR_COMUN)/csv.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_OBJ)/cliente_fifo.o: $(DIR_FIFO)/cliente_fifo.c $(DIR_COMUN)/protocolo.h $(DIR_COMUN)/utilidades.h $(DIR_COMUN)/interfaz.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_BIN)/servidor_fifo: $(DIR_OBJ)/servidor_fifo.o $(OBJS_COMUN_BASE)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(DIR_BIN)/cliente_fifo: $(DIR_OBJ)/cliente_fifo.o $(OBJS_COMUN_BASE) $(OBJS_CLI)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Parte B: memoria compartida (SHM)
$(DIR_OBJ)/servidor_shm.o: $(DIR_SHM)/servidor_shm.c $(DIR_COMUN)/protocolo.h $(DIR_COMUN)/utilidades.h $(DIR_COMUN)/csv.h $(DIR_COMUN)/semaforos.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_OBJ)/cliente_shm.o: $(DIR_SHM)/cliente_shm.c $(DIR_COMUN)/protocolo.h $(DIR_COMUN)/utilidades.h $(DIR_COMUN)/interfaz.h $(DIR_COMUN)/semaforos.h
	$(CC) $(CFLAGS) -c $< -o $@

$(DIR_BIN)/servidor_shm: $(DIR_OBJ)/servidor_shm.o $(OBJS_COMUN_BASE) $(OBJS_SEM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(DIR_BIN)/cliente_shm: $(DIR_OBJ)/cliente_shm.o $(OBJS_COMUN_BASE) $(OBJS_CLI) $(OBJS_SEM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)


# Reglas auxiliares
clean:
	rm -rf $(DIR_OBJ) $(DIR_BIN)

limpiar-ipc:
	@echo "Limpiando FIFOs en /tmp/..."
	rm -f /tmp/fifo_req_$(GRUPO) /tmp/fifo_resp_$(GRUPO) /tmp/fifo_lock_$(GRUPO)
	@echo "Limpiando memoria compartida y semaforos en /dev/shm/..."
	rm -f /dev/shm/shm_$(GRUPO) /dev/shm/sem.sem_*_$(GRUPO)
	@echo "Recursos IPC del grupo '$(GRUPO)' eliminados."

ayuda:
	@echo "Opciones disponibles:"
	@echo "  make              - Compila los 4 ejecutables en bin/"
	@echo "  make clean        - Elimina bin/ y obj/"
	@echo "  make limpiar-ipc  - Borra FIFOs y objetos /dev/shm huerfanos"
	@echo "  make GRUPO=X      - Compila usando 'X' como sufijo de recursos IPC"