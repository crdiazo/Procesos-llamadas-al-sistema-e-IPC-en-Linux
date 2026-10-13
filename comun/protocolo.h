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
#define TAM_TEXTO 128
#define TAM_NOMBRE 64
#define TAM_CATEGORIA 32
#define TAM_FECHA 11 /* "AAAA-MM-DD" + '\0' */
#define TAM_MENSAJE_ERROR 256
#define TAM_MAX_CARGA 512  /* carga util maxima de un mensaje FIFO */
#define LIMITE_POR_DEFECTO 25
#define REGISTROS_POR_BLOQUE 64   /* registros por bloque en la region SHM */
#define SEGUNDOS_ESPERA_CONEXION 5 /* reintentos del cliente FIFO al conectar */
#define SEGUNDOS_VIGILANCIA 2    /* periodo para comprobar si el otro vive */

/* Codigos de operación (campo solicitud_t.operacion). */
typedef enum {
    OP_BUSCAR_CATEGORIA = 1,
    OP_BUSCAR_PRECIO = 2,
    OP_BUSCAR_NOMBRE = 3,
    OP_DESCONECTAR = 8,
    OP_TERMINAR = 9
} codigo_operacion_t;

/* Tipos de respuesta servidor -> cliente por la FIFO. */
typedef enum {
    RESP_REGISTRO = 1,
    RESP_FIN = 2,
    RESP_ERROR = 3,
    RESP_ADIOS = 4
} codigo_respuesta_t;

/* Estados de la region compartida. */
typedef enum {
    EST_LIBRE = 0,
    EST_SOLICITUD = 1,
    EST_BLOQUE = 2,
    EST_FIN = 3,
    EST_ERROR = 4,
    EST_ADIOS = 5
} estado_region_t;

/*
Peticion del cliente (160 bytes en x86-64). Viaja tal cual por la FIFO
o se copia en la region compartida.
 */
typedef struct {
    uint32_t magia; /* MAGIA_PROTOCOLO se aseguara que los bytes que llegan realmente son validos */
    int32_t operacion; /* codigo_operacion_t */
    int32_t pid_cliente;
    int32_t limite; /* 0 = sin límite */
    double precio_min;
    double precio_max;
    char texto[TAM_TEXTO];
} solicitud_t;

/* Un registro del CSV ya interpretado (128 bytes). */
typedef struct {
    int64_t id;
    char nombre[TAM_NOMBRE];
    char categoria[TAM_CATEGORIA];
    double precio;
    int32_t stock;
    char fecha[TAM_FECHA];
} registro_t;

/* Resumen al final de cada busqueda (32 bytes). */
typedef struct {
    int64_t lineas_leidas;
    int32_t coincidencias;
    int32_t enviados;
    int32_t lineas_invalidas; /* lineas con formato incorrecto (ignoradas) */
    double  milisegundos; /* tiempo de la busqueda en el servidor */
} resumen_t;

/*
Cabecera de cada mensaje FIFO servidor -> cliente (8 bytes).
Inmediatamente despues vienen 'longitud' bytes de carga útil.
 */
typedef struct {
    int32_t tipo; /* codigo_respuesta_t */
    int32_t longitud; /* 0 <= longitud <= TAM_MAX_CARGA */
} cabecera_respuesta_t;

/*
Region de memoria compartida. Los campos de comunicacion están protegidos
por el protocolo de semaforos descrito en shm/servidor_shm.c.
*/
typedef struct {
    /* Estado del servidor (escrito solo por el servidor) */
    uint32_t magia;
    int32_t pid_servidor;
    int32_t servidor_activo;

    /* Estado de la comunicacion */
    int32_t estado; /* estado_region_t */
    int32_t num_registros; /* registros validos en 'registros' */
    int32_t numero_bloque;

    /* Peticion escrita por el cliente que tiene el turno */
    solicitud_t solicitud;

    /* Respuesta escrita por el servidor */
    resumen_t resumen; /* valido con EST_FIN */
    char mensaje_error[TAM_MENSAJE_ERROR]; /* valido con EST_ERROR */
    registro_t registros[REGISTROS_POR_BLOQUE]; /* bloque de resultados */
} region_compartida_t;

/*
POSIX garantiza atomicidad de write() en FIFO hasta PIPE_BUF (>= 512,
4096 en Linux). Si estos asserts fallan, dos mensajes podrian mezclarse.
 */
_Static_assert(sizeof(solicitud_t) <= 512,
               "la solicitud debe caber en PIPE_BUF mínimo (512)");
_Static_assert(sizeof(registro_t) <= TAM_MAX_CARGA, "registro demasiado grande");
_Static_assert(sizeof(resumen_t)  <= TAM_MAX_CARGA, "resumen demasiado grande");
_Static_assert(TAM_MENSAJE_ERROR  <= TAM_MAX_CARGA, "mensaje de error grande");
_Static_assert(sizeof(cabecera_respuesta_t) + TAM_MAX_CARGA <= 4096,
               "un mensaje de respuesta debe caber en PIPE_BUF de Linux");

#endif /* PROTOCOLO_H */