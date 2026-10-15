/*
 * ============================================================================
 *  cliente_fifo.c  —  Parte A: cliente con tuberías nombradas (FIFO)
 *
 *  Uso:   ./bin/cliente_fifo        (el servidor debe estar en ejecución)
 *
 *  Funcionamiento:
 *    1. Toma un cerrojo exclusivo fcntl(F_SETLK) sobre FIFO_CERROJO para que
 *       sólo un cliente a la vez use el par de FIFO (si hubiera dos, sus
 *       respuestas se mezclarían). El núcleo libera el cerrojo si el cliente
 *       muere.
 *    2. Abre FIFO_SOLICITUD en escritura con O_NONBLOCK y reintentos:
 *         ENOENT -> la FIFO no existe (servidor nunca arrancó).
 *         ENXIO  -> existe pero nadie la lee (servidor ausente).
 *    3. Abre FIFO_RESPUESTA en lectura (bloqueante: cita con el servidor).
 *    4. Menú: envía solicitud_t y lee mensajes cabecera+carga hasta recibir
 *       RESP_FIN, RESP_ERROR o RESP_ADIOS.
 *
 *  El cliente NO crea ni elimina las FIFO: son propiedad del servidor.
 * ============================================================================
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../comun/interfaz.h"
#include "../comun/protocolo.h"
#include "../comun/utilidades.h"

/* Única variable global: bandera escrita por el manejador de señales */
static volatile sig_atomic_t terminar_cliente = 0;

/* Descriptores que mantiene abiertos el cliente durante la sesión */
typedef struct {
    int fd_cerrojo;    /* archivo de cerrojo (exclusión entre clientes) */
    int fd_solicitud;  /* FIFO_SOLICITUD abierta en O_WRONLY            */
    int fd_respuesta;  /* FIFO_RESPUESTA abierta en O_RDONLY            */
} conexion_fifo_t;

/* Resultado de recibir una respuesta completa */
typedef enum {
    RESPUESTA_COMPLETA =  0,  /* RESP_FIN o RESP_ERROR recibido      */
    RESPUESTA_ADIOS    =  1,  /* RESP_ADIOS recibido                 */
    RESPUESTA_FALLO    = -1   /* error de E/S, protocolo o señal     */
} resultado_respuesta_t;

/* ---------------------------------------------------------------------------
 * manejar_senal_terminacion: sólo activa la bandera.
 * ------------------------------------------------------------------------- */
static void manejar_senal_terminacion(int numero_senal)
{
    (void) numero_senal;
    terminar_cliente = 1;
}

/* ---------------------------------------------------------------------------
 * adquirir_cerrojo_cliente: cerrojo de escritura sobre todo el archivo.
 * Si otro cliente lo tiene, se informa y se espera (F_SETLKW, interrumpible).
 * ------------------------------------------------------------------------- */
static int adquirir_cerrojo_cliente(void)
{
    struct flock cerrojo;
    int fd = open(FIFO_CERROJO, O_RDWR | O_CREAT, 0600);

    if (fd == -1) {
        perror("open(" FIFO_CERROJO ")");
        return -1;
    }
    memset(&cerrojo, 0, sizeof cerrojo);
    cerrojo.l_type = F_WRLCK;
    cerrojo.l_whence = SEEK_SET;
    cerrojo.l_start = 0;
    cerrojo.l_len = 0; /* todo el archivo */

    if (fcntl(fd, F_SETLK, &cerrojo) == 0) {
        return fd;
    }
    if (errno != EACCES && errno != EAGAIN) {
        perror("fcntl(F_SETLK)");
        cerrar_descriptor(fd, "cerrojo");
        return -1;
    }

    printf("Otro cliente está usando el servidor. Esperando turno "
           "(Ctrl+C para cancelar)...\n");
    while (fcntl(fd, F_SETLKW, &cerrojo) == -1) {
        if (errno == EINTR && !terminar_cliente) {
            continue;
        }
        if (errno == EINTR) {
            fprintf(stderr, "Espera cancelada por el usuario.\n");
        } else {
            perror("fcntl(F_SETLKW)");
        }
        cerrar_descriptor(fd, "cerrojo");
        return -1;
    }
    return fd;
}

/* ---------------------------------------------------------------------------
 * cerrar_conexion: cierra todos los descriptores abiertos. Al cerrar el
 * extremo de escritura de FIFO_SOLICITUD, el servidor recibe EOF.
 * ------------------------------------------------------------------------- */
static void cerrar_conexion(conexion_fifo_t *conexion)
{
    cerrar_descriptor(conexion->fd_solicitud, "FIFO de solicitudes");
    cerrar_descriptor(conexion->fd_respuesta, "FIFO de respuestas");
    cerrar_descriptor(conexion->fd_cerrojo, "cerrojo"); /* libera el cerrojo */
    conexion->fd_solicitud = conexion->fd_respuesta = conexion->fd_cerrojo = -1;
}

/* ---------------------------------------------------------------------------
 * conectar_con_servidor: secuencia de apertura. El orden (primero
 * solicitudes, luego respuestas) coincide con el del servidor para evitar
 * un interbloqueo en los open() bloqueantes.
 * ------------------------------------------------------------------------- */
static int conectar_con_servidor(conexion_fifo_t *conexion)
{
    conexion->fd_cerrojo = conexion->fd_solicitud = conexion->fd_respuesta = -1;

    conexion->fd_cerrojo = adquirir_cerrojo_cliente();
    if (conexion->fd_cerrojo == -1) {
        return -1;
    }

    printf("Conectando con el servidor por %s ...\n", FIFO_SOLICITUD);
    conexion->fd_solicitud = abrir_fifo_escritura_con_espera(
        FIFO_SOLICITUD, SEGUNDOS_ESPERA_CONEXION, &terminar_cliente);
    if (conexion->fd_solicitud == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: la FIFO %s no existe. ¿Está en ejecución "
                    "servidor_fifo?\n", FIFO_SOLICITUD);
        } else if (errno == ENXIO) {
            fprintf(stderr, "Error: ningún proceso lee %s; el servidor no está "
                    "en ejecución (FIFO huérfana).\n", FIFO_SOLICITUD);
        } else if (errno == EINTR) {
            fprintf(stderr, "Conexión cancelada.\n");
        } else {
            perror("open(FIFO de solicitudes)");
        }
        return -1;
    }

    /* Bloqueante: termina cuando el servidor abre la FIFO en escritura */
    while ((conexion->fd_respuesta = open(FIFO_RESPUESTA, O_RDONLY)) == -1) {
        if (errno == EINTR && !terminar_cliente) {
            continue;
        }
        if (errno == EINTR) {
            fprintf(stderr, "Conexión cancelada.\n");
        } else {
            perror("open(FIFO de respuestas)");
        }
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * enviar_solicitud: una única write() de 160 bytes (< PIPE_BUF => atómica).
 * ------------------------------------------------------------------------- */
static int enviar_solicitud(int fd, const solicitud_t *solicitud)
{
    for (;;) {
        if (write_all(fd, solicitud, sizeof *solicitud) != -1) {
            return 0;
        }
        if (errno == EINTR && !terminar_cliente) {
            continue;
        }
        if (errno == EPIPE) {
            fprintf(stderr, "Error: el servidor ya no lee solicitudes (EPIPE); "
                    "probablemente terminó.\n");
        } else if (errno != EINTR) {
            perror("write(FIFO de solicitudes)");
        }
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * leer_mensaje: lee exactamente 'bytes' de la FIFO de respuestas.
 * Devuelve 0 si se completó o -1 (con el motivo ya informado).
 * ------------------------------------------------------------------------- */
static int leer_mensaje(int fd, void *destino, size_t bytes)
{
    for (;;) {
        ssize_t leidos = read_all(fd, destino, bytes);

        if (leidos == (ssize_t) bytes) {
            return 0;
        }
        if (leidos == -1 && errno == EINTR && !terminar_cliente) {
            continue;
        }
        if (leidos == -1 && errno == EINTR) {
            fprintf(stderr, "\nLectura interrumpida por señal.\n");
        } else if (leidos == -1) {
            perror("read(FIFO de respuestas)");
        } else if (leidos == 0) {
            fprintf(stderr, "Error: EOF en la FIFO de respuestas; el servidor "
                    "cerró la conexión o terminó.\n");
        } else {
            fprintf(stderr, "Error de protocolo: mensaje truncado (%zd de %zu "
                    "bytes).\n", leidos, bytes);
        }
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * recibir_respuesta: procesa mensajes hasta el final de la respuesta.
 *   RESP_REGISTRO (carga = registro_t)  -> se imprime y se sigue leyendo.
 *   RESP_FIN      (carga = resumen_t)   -> fin normal.
 *   RESP_ERROR    (carga = texto)       -> fin con error del servidor.
 *   RESP_ADIOS    (sin carga)           -> confirmación de salida.
 * ------------------------------------------------------------------------- */
static resultado_respuesta_t recibir_respuesta(int fd)
{
    unsigned char carga[TAM_MAX_CARGA];
    int encabezado_impreso = 0;

    for (;;) {
        cabecera_respuesta_t cabecera;

        if (leer_mensaje(fd, &cabecera, sizeof cabecera) == -1) {
            return RESPUESTA_FALLO;
        }
        if (cabecera.longitud < 0 || cabecera.longitud > TAM_MAX_CARGA) {
            fprintf(stderr, "Error de protocolo: longitud inválida (%d).\n",
                    (int) cabecera.longitud);
            return RESPUESTA_FALLO;
        }
        if (cabecera.longitud > 0 &&
            leer_mensaje(fd, carga, (size_t) cabecera.longitud) == -1) {
            return RESPUESTA_FALLO;
        }

        switch (cabecera.tipo) {
        case RESP_REGISTRO: {
            registro_t registro;
            if (cabecera.longitud != (int32_t) sizeof registro) {
                fprintf(stderr, "Error de protocolo: RESP_REGISTRO de %d bytes.\n",
                        (int) cabecera.longitud);
                return RESPUESTA_FALLO;
            }
            memcpy(&registro, carga, sizeof registro); /* evita accesos desalineados */
            if (!encabezado_impreso) {
                imprimir_encabezado_resultados();
                encabezado_impreso = 1;
            }
            imprimir_registro(&registro);
            break;
        }
        case RESP_FIN: {
            resumen_t resumen;
            if (cabecera.longitud != (int32_t) sizeof resumen) {
                fprintf(stderr, "Error de protocolo: RESP_FIN de %d bytes.\n",
                        (int) cabecera.longitud);
                return RESPUESTA_FALLO;
            }
            memcpy(&resumen, carga, sizeof resumen);
            imprimir_resumen(&resumen);
            return RESPUESTA_COMPLETA;
        }
        case RESP_ERROR:
            /* Garantizar terminación en '\0' aunque el servidor fallara */
            carga[cabecera.longitud > 0 ? cabecera.longitud - 1 : 0] = '\0';
            fprintf(stderr, "El servidor informó un error: %s\n", (char *) carga);
            return RESPUESTA_COMPLETA;
        case RESP_ADIOS:
            return RESPUESTA_ADIOS;
        default:
            fprintf(stderr, "Error de protocolo: tipo de mensaje desconocido (%d).\n",
                    (int) cabecera.tipo);
            return RESPUESTA_FALLO;
        }
    }
}

/* ---------------------------------------------------------------------------
 * main: bucle del menú.
 * ------------------------------------------------------------------------- */
int main(void)
{
    conexion_fifo_t conexion;
    int limite = LIMITE_POR_DEFECTO;
    int codigo_salida = EXIT_SUCCESS;
    int continuar = 1;

    if (instalar_manejadores_terminacion(manejar_senal_terminacion) == -1) {
        return EXIT_FAILURE;
    }
    printf("[cliente_fifo] PID=%d PPID=%d\n", (int) getpid(), (int) getppid());

    if (conectar_con_servidor(&conexion) == -1) {
        cerrar_conexion(&conexion);
        return EXIT_FAILURE;
    }
    printf("[cliente_fifo] Conectado (fd cerrojo=%d, fd solicitudes=%d, "
           "fd respuestas=%d)\n", conexion.fd_cerrojo, conexion.fd_solicitud,
           conexion.fd_respuesta);

    while (continuar && !terminar_cliente) {
        long opcion;
        solicitud_t solicitud;

        mostrar_menu("FIFO", limite);
        if (leer_entero("Seleccione una opción: ", 0, OPCION_MAXIMA, &opcion) == -1) {
            break; /* EOF o señal: al cerrar, el servidor verá EOF */
        }

        if (opcion == OPCION_LIMITE) {
            long nuevo_limite;
            if (leer_entero("Nuevo límite (0 = sin límite): ", 0, 1000000,
                            &nuevo_limite) == 0) {
                limite = (int) nuevo_limite;
            }
            continue;
        }

        if (construir_solicitud((int) opcion, limite, &solicitud) == -1) {
            break;
        }
        if (enviar_solicitud(conexion.fd_solicitud, &solicitud) == -1) {
            codigo_salida = EXIT_FAILURE;
            break;
        }

        switch (recibir_respuesta(conexion.fd_respuesta)) {
        case RESPUESTA_COMPLETA:
            break;
        case RESPUESTA_ADIOS:
            printf("[cliente_fifo] El servidor confirmó %s.\n",
                   opcion == OPCION_DETENER ? "su terminación" : "la desconexión");
            continuar = 0;
            break;
        case RESPUESTA_FALLO:
            codigo_salida = EXIT_FAILURE;
            continuar = 0;
            break;
        }
    }

    if (terminar_cliente) {
        printf("\n[cliente_fifo] Señal recibida: cerrando ordenadamente.\n");
    }
    cerrar_conexion(&conexion);
    printf("[cliente_fifo] Descriptores cerrados. Fin.\n");
    return codigo_salida;
}
