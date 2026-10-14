/*
   Utilidades de sincronización para la versión de memoria compartida.
 */
#ifndef SEMAFOROS_H
#define SEMAFOROS_H

#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>

/* Resultados posibles de esperar_semaforo_vigilado() */
typedef enum {
    ESPERA_OK              =  0,  
    ESPERA_ERROR           = -1,  
    ESPERA_INTERRUMPIDA    = -2,  
    ESPERA_PROCESO_AUSENTE = -3   
} resultado_espera_t;

/*
    Deja al proceso dormido en el semáforo hasta que llegue un aviso.
    No está dando vueltas ni gastando CPU: duerme de verdad.
    Cada "segundos_intervalo" segundos se despierta un momento para mirar dos cosas:
    si llegó una señal y hay que terminar,
    si el proceso 'pid_vigilado' sigue vivo (si pasas 0, no se vigila a nadie).
 
    La idea es que un cliente no se quede esperando para siempre si el servidor
    se muere, y al revés también.
 */

resultado_espera_t esperar_semaforo_vigilado(sem_t *semaforo, pid_t pid_vigilado,
                                             volatile sig_atomic_t *bandera_terminar,
                                             int segundos_intervalo);

void vaciar_semaforo(sem_t *semaforo);

#endif
