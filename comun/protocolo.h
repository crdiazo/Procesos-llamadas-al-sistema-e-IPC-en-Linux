/*
 * protocolo.h — Definiciones compartidas entre cliente y servidor.
 */

#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>

/* Sufijo de todos los recursos IPC, permite varios grupos por maquina */
#ifndef NOMBRE_GRUPO
#define NOMBRE_GRUPO "Los_illuminati"
#endif

#define RUTA_CSV_POR_DEFECTO "datos.csv"

/* Recursos IPC - Version FIFO */
#define FIFO_SOLICITUD "/tmp/fifo_req_" NOMBRE_GRUPO   /* Cliente -> Servidor */
#define FIFO_RESPUESTA "/tmp/fifo_resp_" NOMBRE_GRUPO  /* Servidor -> Cliente */
#define FIFO_CERROJO "/tmp/fifo_lock_" NOMBRE_GRUPO    /* Exclusion entre clientes */

/* Recursos IPC - Version memoria compartida (shm) */
#define SHM_NOMBRE "/shm_" NOMBRE_GRUPO
#define SEM_TURNO "/sem_turno_" NOMBRE_GRUPO          /* valor inicial 1 */
#define SEM_SOLICITUD "/sem_solicitud_" NOMBRE_GRUPO  /* valor inicial 0 */
#define SEM_RESPUESTA "/sem_respuesta_" NOMBRE_GRUPO  /* valor inicial 0 */
#define SEM_CONSUMIDO "/sem_consumido_" NOMBRE_GRUPO  /* valor inicial 0 */

/* Numero magico "PRC1": identifica mensajes y regiones validas */
#define MAGIA_PROTOCOLO 0x31435250u

/* Tamaños de campos y limites del protocolo */
#define TAM_TEXTO            128
#define TAM_NOMBRE           64
#define TAM_CATEGORIA        32
#define TAM_FECHA            11   /* "AAAA-MM-DD" + '\0'                      */
#define TAM_MENSAJE_ERROR    256
#define TAM_MAX_CARGA        512  /* carga util maxima de un mensaje FIFO    */
#define LIMITE_POR_DEFECTO   25
#define REGISTROS_POR_BLOQUE 64   /* registros por bloque en la region SHM   */
#define SEGUNDOS_ESPERA_CONEXION 5 /* reintentos del cliente FIFO al conectar */
#define SEGUNDOS_VIGILANCIA  2    /* periodo para comprobar si el otro vive  */


#endif /* PROTOCOLO_H */