/*
utilidades.h — Funciones auxiliares de bajo nivel para cliente y servidor.
 */
#ifndef UTILIDADES_H
#define UTILIDADES_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/*
Escribe exactamente 'bytes' bytes en 'fd', reintentando ante escrituras parciales.
Devuelve 'bytes' o -1 (errno indica la causa)
*/
ssize_t write_all(int fd, const void *buffer, size_t bytes);

/* Lee exactamente 'bytes' bytes de 'fd'.
Devuelve 'bytes', o 0 si el otro extremo cerro, o -1 en error
*/
ssize_t read_all(int fd, void *buffer, size_t bytes);

/*
Abre una FIFO para escritura con O_NONBLOCK y reintentos
(1 por segundo, hasta 'segundos_maximos'). Devuelve fd o -1 (ENOENT, ENXIO, EINTR, ...)
*/
int abrir_fifo_escritura_con_espera(const char *ruta, int segundos_maximos,
                                    volatile sig_atomic_t *bandera_terminar);

/* Quita O_NONBLOCK de un descriptor. Devuelve 0 o -1. */
int quitar_no_bloqueante(int fd);

/*
Instala 'manejador' para SIGINT y SIGTERM sin SA_RESTART (para que las
llamadas bloqueantes devuelvan EINTR) e ignora SIGPIPE. Devuelve 0 o -1
*/
int instalar_manejadores_terminacion(void (*manejador)(int));

/* close() con log de error. Devuelve 0 o -1. */
int cerrar_descriptor(int fd, const char *descripcion);

/* Devuelve 1 si existe un proceso con ese PID (kill(pid, 0)), 0 si no. */
int proceso_existe(pid_t pid);

/* Copia acotada que siempre deja la cadena terminada en '\0'. */
void copiar_cadena(char *destino, size_t tam_destino, const char *origen);

/* Devuelve 1 si 'patron' aparece en 'texto' sin distinguir mayusculas. */
int contiene_sin_mayusculas(const char *texto, const char *patron);

/* Milisegundos transcurridos desde 'inicio' (reloj CLOCK_MONOTONIC). */
double milisegundos_transcurridos(const struct timespec *inicio);

#endif /* UTILIDADES_H */