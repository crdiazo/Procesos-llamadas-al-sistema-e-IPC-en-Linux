/*
    Implementación de las utilidades de sincronización (semáforos POSIX).
*/
#define _POSIX_C_SOURCE 200809L

#include "semaforos.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "utilidades.h"

/* 
    Por medio de un ciclo sem_timedwait recibe un instante ABSOLUTO medido con CLOCK_REALTIME.
    Mientras espera, el proceso está en estado S (dormido en un futex del núcleo): 
    no consume CPU, por lo que no hay espera activa.Por eso mismo se usa el ciclo
    para volver a activar el semaforo.

    Cada vez que vence el timeout comprobamos si el otro proceso sigue vivo.
    Si se murió, salimos con ESPERA_PROCESO_AUSENTE para no quedarnos colgados
    esperando un aviso que ya nunca va a llegar.
*/
resultado_espera_t esperar_semaforo_vigilado(sem_t *semaforo, pid_t pid_vigilado,
                                             volatile sig_atomic_t *bandera_terminar,
                                             int segundos_intervalo)
{
    for (;;) {
        struct timespec limite;

        if (bandera_terminar != NULL && *bandera_terminar) {
            return ESPERA_INTERRUMPIDA;
        }
        if (clock_gettime(CLOCK_REALTIME, &limite) == -1) {
            perror("clock_gettime");
            return ESPERA_ERROR;
        }
        limite.tv_sec += segundos_intervalo;

        if (sem_timedwait(semaforo, &limite) == 0) {
            return ESPERA_OK;
        }
        if (errno == EINTR) {
            continue; 
        }
        if (errno == ETIMEDOUT) {
            if (pid_vigilado > 0 && !proceso_existe(pid_vigilado)) {
                return ESPERA_PROCESO_AUSENTE;
            }
            continue;
        }
        perror("sem_timedwait");
        return ESPERA_ERROR;
    }
}

/* 
    Deja el semáforo en 0 sin bloquear. 
*/
void vaciar_semaforo(sem_t *semaforo)
{
    while (sem_trywait(semaforo) == 0) {
        /* descartar un aviso pendiente */
    }
}
