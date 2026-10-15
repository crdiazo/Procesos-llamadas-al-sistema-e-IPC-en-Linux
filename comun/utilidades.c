/* utilidades.c: implementacion de las funciones declaradas en utilidades.h */
#define _POSIX_C_SOURCE 200809L

#include "utilidades.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* write() puede escribir menos de lo pedido, asi que se repite hasta completar. */
ssize_t write_all(int fd, const void *buffer, size_t bytes)
{
    const unsigned char *cursor = buffer;
    size_t total = 0;

    while (total < bytes) {
        ssize_t escritos = write(fd, cursor + total, bytes - total);
        if (escritos == -1) {
            /* Con el mensaje a medias se continua para no corromperlo */
            if (errno == EINTR && total > 0) {
                continue;
            }
            return -1;
        }
        if (escritos == 0) {
            errno = EIO;
            return -1;
        }
        total += (size_t) escritos;
    }
    return (ssize_t) total;
}

/*
read() en una FIFO devuelve lo que haya, que puede ser menos que un mensaje
completo, por eso se acumula hasta completar 'bytes'
*/
ssize_t read_all(int fd, void *buffer, size_t bytes)
{
    unsigned char *cursor = buffer;
    size_t total = 0;

    while (total < bytes) {
        ssize_t leidos = read(fd, cursor + total, bytes - total);
        if (leidos == -1) {
            if (errno == EINTR && total > 0) {
                continue;
            }
            return -1;
        }
        if (leidos == 0) {
            /* Fin de archivo: nadie tiene la FIFO abierta en escritura */
            break;
        }
        total += (size_t) leidos;
    }
    return (ssize_t) total;
}

int quitar_no_bloqueante(int fd)
{
    int indicadores = fcntl(fd, F_GETFL);
    if (indicadores == -1) {
        perror("fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, indicadores & ~O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return -1;
    }
    return 0;
}

/*
Con O_NONBLOCK, open() falla con ENXIO si no hay lector y con ENOENT si la
FIFO no existe, asi el cliente no queda bloqueado para siempre
*/
int abrir_fifo_escritura_con_espera(const char *ruta, int segundos_maximos,
                                    volatile sig_atomic_t *bandera_terminar)
{
    int intentos = 0;

    for (;;) {
        int fd = open(ruta, O_WRONLY | O_NONBLOCK);
        if (fd != -1) {
            if (quitar_no_bloqueante(fd) == -1) {
                int error_guardado = errno;
                close(fd);
                errno = error_guardado;
                return -1;
            }
            return fd;
        }
        /* Cualquier otro error (ENOENT, EACCES, ...) es definitivo */
        if (errno != ENXIO && errno != EINTR) {
            return -1;
        }
        if (bandera_terminar != NULL && *bandera_terminar) {
            errno = EINTR;
            return -1;
        }
        if (intentos >= segundos_maximos) {
            errno = ENXIO;
            return -1;
        }
        intentos++;
        fprintf(stderr, "  Servidor no disponible todavía; reintento %d/%d...\n",
                intentos, segundos_maximos);
        sleep(1); /* espera pasiva: no consume CPU */
    }
}

/*
sigaction() en lugar de signal() para controlar la semantica: sin
SA_RESTART, las llamadas bloqueantes devuelven EINTR al llegar una señal
*/
int instalar_manejadores_terminacion(void (*manejador)(int))
{
    struct sigaction accion;
    struct sigaction ignorar;

    memset(&accion, 0, sizeof accion);
    accion.sa_handler = manejador;
    accion.sa_flags = 0;
    if (sigemptyset(&accion.sa_mask) == -1) {
        perror("sigemptyset");
        return -1;
    }
    if (sigaction(SIGINT, &accion, NULL) == -1) {
        perror("sigaction(SIGINT)");
        return -1;
    }
    if (sigaction(SIGTERM, &accion, NULL) == -1) {
        perror("sigaction(SIGTERM)");
        return -1;
    }

    /* Con SIGPIPE ignorada, escribir sin lector devuelve EPIPE */
    memset(&ignorar, 0, sizeof ignorar);
    ignorar.sa_handler = SIG_IGN;
    if (sigemptyset(&ignorar.sa_mask) == -1) {
        perror("sigemptyset");
        return -1;
    }
    if (sigaction(SIGPIPE, &ignorar, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        return -1;
    }
    return 0;
}

int cerrar_descriptor(int fd, const char *descripcion)
{
    if (fd < 0) {
        return 0;
    }
    if (close(fd) == -1) {
        fprintf(stderr, "close(%s): %s\n", descripcion, strerror(errno));
        return -1;
    }
    return 0;
}

/* kill(pid, 0) no envia nada, solo comprueba. EPERM: existe, pero es de otro usuario. */
int proceso_existe(pid_t pid)
{
    if (pid <= 0) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
}

void copiar_cadena(char *destino, size_t tam_destino, const char *origen)
{
    size_t i = 0;

    if (tam_destino == 0) {
        return;
    }
    while (i + 1 < tam_destino && origen[i] != '\0') {
        destino[i] = origen[i];
        i++;
    }
    destino[i] = '\0';
}

/* Busqueda ingenua, solo ASCII. Un patron vacio siempre coincide. */
int contiene_sin_mayusculas(const char *texto, const char *patron)
{
    size_t i;
    size_t j;

    if (patron[0] == '\0') {
        return 1;
    }
    for (i = 0; texto[i] != '\0'; i++) {
        for (j = 0; patron[j] != '\0' && texto[i + j] != '\0'; j++) {
            if (tolower((unsigned char) texto[i + j]) !=
                tolower((unsigned char) patron[j])) {
                break;
            }
        }
        if (patron[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

double milisegundos_transcurridos(const struct timespec *inicio)
{
    struct timespec ahora;

    if (clock_gettime(CLOCK_MONOTONIC, &ahora) == -1) {
        return 0.0;
    }
    return (double) (ahora.tv_sec - inicio->tv_sec) * 1000.0 +
           (double) (ahora.tv_nsec - inicio->tv_nsec) / 1.0e6;
}