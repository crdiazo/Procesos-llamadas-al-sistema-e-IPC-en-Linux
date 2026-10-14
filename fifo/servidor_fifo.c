
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun/csv.h"
#include "../comun/protocolo.h"
#include "../comun/utilidades.h"

#define PREFIJO "[servidor_fifo]"

/*
 * Bandera de terminación. Es la ÚNICA variable global: un manejador de
 * señales sólo puede comunicarse con el resto del programa a través de una
 * variable de tipo volatile sig_atomic_t.
 */
static volatile sig_atomic_t terminar_servidor = 0;

/* Resultado de atender una sesión completa con un cliente */
typedef enum {
    SESION_CLIENTE_DESCONECTADO, /* volver a esperar otro cliente */
    SESION_DETENER_SERVIDOR      /* OP_TERMINAR o señal recibida  */
} resultado_sesion_t;

/* Resultado de atender una solicitud individual */
typedef enum {
    SOLICITUD_ATENDIDA,
    SOLICITUD_DESCONECTAR,
    SOLICITUD_DETENER,
    SOLICITUD_FALLO_ENVIO
} resultado_solicitud_t;

/* Contexto que recibe el callback de envío de registros */
typedef struct {
    int fd_respuesta;  /* extremo de escritura de FIFO_RESPUESTA          */
    int errno_envio;   /* errno del último fallo de escritura (0 = ninguno)*/
} contexto_envio_t;

//activa la bandera (async-signal-safe).
static void manejar_senal_terminacion(int numero_senal)
{
    (void) numero_senal;
    terminar_servidor = 1;
}

//crea la FIFO o reutiliza una existente si realmente es FIFO.
// Se usa lstat() para no seguir enlaces simbólicos en /tmp.
static int crear_fifo(const char *ruta)
{
    struct stat informacion;

    if (mkfifo(ruta, 0600) == 0) {
        printf(PREFIJO " FIFO creada: %s\n", ruta);
        return 0;
    }
    if (errno != EEXIST) {
        fprintf(stderr, PREFIJO " mkfifo(%s): %s\n", ruta, strerror(errno));
        return -1;
    }
    if (lstat(ruta, &informacion) == -1) {
        fprintf(stderr, PREFIJO " lstat(%s): %s\n", ruta, strerror(errno));
        return -1;
    }
    if (!S_ISFIFO(informacion.st_mode)) {
        fprintf(stderr, PREFIJO " %s existe pero NO es una FIFO; elimínelo "
                "manualmente.\n", ruta);
        return -1;
    }
    printf(PREFIJO " Reutilizando FIFO existente: %s\n", ruta);
    return 0;
}

// unlink() de las FIFO y del archivo de cerrojo.
// Tras unlink el nombre desaparece del sistema de archivos; si algún proceso aún tuviera la FIFO abierta, el objeto del núcleo vive hasta su close().
static void eliminar_recursos_fifo(void)
{
    const char *rutas[] = { FIFO_SOLICITUD, FIFO_RESPUESTA, FIFO_CERROJO };
    size_t i;

    for (i = 0; i < sizeof rutas / sizeof rutas[0]; i++) {
        if (unlink(rutas[i]) == 0) {
            printf(PREFIJO " Eliminado: %s\n", rutas[i]);
        } else if (errno != ENOENT) {
            fprintf(stderr, PREFIJO " unlink(%s): %s\n", rutas[i], strerror(errno));
        }
    }
}

/* compone cabecera + carga en un único búfer y lo escribe con write_all. Al ser <= PIPE_BUF, el núcleo garantiza que la escritura es
 * atómica: el lector nunca verá un mensaje mezclado con otro.
 * Devuelve 0 o -1 (errno = EPIPE si el cliente ya no lee).
*/
static int enviar_mensaje(int fd, int32_t tipo, const void *carga, size_t longitud)
{
    unsigned char paquete[sizeof(cabecera_respuesta_t) + TAM_MAX_CARGA];
    cabecera_respuesta_t cabecera;

    if (longitud > TAM_MAX_CARGA) {
        errno = EMSGSIZE;
        return -1;
    }
    cabecera.tipo = tipo;
    cabecera.longitud = (int32_t) longitud;
    memcpy(paquete, &cabecera, sizeof cabecera);
    if (longitud > 0) {
        memcpy(paquete + sizeof cabecera, carga, longitud);
    }

    for (;;) {
        if (write_all(fd, paquete, sizeof cabecera + longitud) != -1) {
            return 0;
        }
        /* EINTR sin haber escrito nada: reintentar salvo que haya que terminar */
        if (errno == EINTR && !terminar_servidor) {
            continue;
        }
        return -1;
    }
}

// mensaje RESP_ERROR con texto terminado en '\0'.
static int enviar_error(int fd, const char *texto)
{
    char mensaje[TAM_MENSAJE_ERROR];

    copiar_cadena(mensaje, sizeof mensaje, texto);
    return enviar_mensaje(fd, RESP_ERROR, mensaje, strlen(mensaje) + 1);
}

// callback de buscar_en_csv; envía un RESP_REGISTRO.
// Devuelve distinto de 0 para abortar la búsqueda (señal o cliente perdido).
static int enviar_registro(const registro_t *registro, void *contexto)
{
    contexto_envio_t *envio = contexto;

    if (terminar_servidor) {
        return -1;
    }
    if (enviar_mensaje(envio->fd_respuesta, RESP_REGISTRO, registro,
                       sizeof *registro) == -1) {
        envio->errno_envio = errno;
        return -1;
    }
    return 0;
}

// ejecuta una solicitud ya validada en su formato.
static resultado_solicitud_t atender_solicitud(int fd_respuesta,
                                               const solicitud_t *solicitud,
                                               const char *ruta_csv)
{
    char mensaje[TAM_MENSAJE_ERROR];
    resumen_t resumen;
    contexto_envio_t envio = { fd_respuesta, 0 };
    int resultado;

    // Órdenes de control: se confirman con RESP_ADIOS 
    if (solicitud->operacion == OP_DESCONECTAR || solicitud->operacion == OP_TERMINAR) {
        if (enviar_mensaje(fd_respuesta, RESP_ADIOS, NULL, 0) == -1) {
            fprintf(stderr, PREFIJO " No se pudo confirmar al cliente: %s\n",
                    strerror(errno));
        }
        return solicitud->operacion == OP_TERMINAR ? SOLICITUD_DETENER
                                                   : SOLICITUD_DESCONECTAR;
    }

    // Validación de parámetros: el servidor no confía en el cliente
    if (validar_solicitud(solicitud, mensaje, sizeof mensaje) == -1) {
        fprintf(stderr, PREFIJO " Solicitud rechazada: %s\n", mensaje);
        return enviar_error(fd_respuesta, mensaje) == 0 ? SOLICITUD_ATENDIDA
                                                        : SOLICITUD_FALLO_ENVIO;
    }

    // Búsqueda secuencial en disco; cada coincidencia se envía al momento
    resultado = buscar_en_csv(ruta_csv, solicitud, enviar_registro, &envio,
                              &resumen, mensaje, sizeof mensaje);
    if (resultado == -1) {
        fprintf(stderr, PREFIJO " %s\n", mensaje);
        return enviar_error(fd_respuesta, mensaje) == 0 ? SOLICITUD_ATENDIDA
                                                        : SOLICITUD_FALLO_ENVIO;
    }
    if (resultado == -2) {
        if (envio.errno_envio != 0) {
            fprintf(stderr, PREFIJO " Envío abortado: %s (¿cliente terminado?)\n",
                    strerror(envio.errno_envio));
        } else {
            fprintf(stderr, PREFIJO " Búsqueda interrumpida por señal.\n");
        }
        return SOLICITUD_FALLO_ENVIO;
    }

    printf(PREFIJO " %d coincidencias, %d enviadas, %lld líneas leídas, %.2f ms\n",
           (int) resumen.coincidencias, (int) resumen.enviados,
           (long long) resumen.lineas_leidas, resumen.milisegundos);

    if (enviar_mensaje(fd_respuesta, RESP_FIN, &resumen, sizeof resumen) == -1) {
        fprintf(stderr, PREFIJO " No se pudo enviar RESP_FIN: %s\n", strerror(errno));
        return SOLICITUD_FALLO_ENVIO;
    }
    return SOLICITUD_ATENDIDA;
}

// atender_sesion: lee solicitudes de un cliente conectado hasta que se
// desconecta, ordena terminar o llega una señal.
static resultado_sesion_t atender_sesion(int fd_solicitud, int fd_respuesta,
                                         const char *ruta_csv)
{
    solicitud_t solicitud;
    char descripcion[256];

    while (!terminar_servidor) {
        // read() se bloquea aquí mientras la FIFO esté vacía y haya escritor
        ssize_t leidos = read_all(fd_solicitud, &solicitud, sizeof solicitud);

        if (leidos == -1) {
            if (errno == EINTR) {
                continue; // el while comprueba la bandera 
            }
            perror(PREFIJO " read(FIFO de solicitudes)");
            return SESION_CLIENTE_DESCONECTADO;
        }
        if (leidos == 0) {
            printf(PREFIJO " EOF en la FIFO de solicitudes: el cliente cerró su "
                   "extremo o terminó.\n");
            return SESION_CLIENTE_DESCONECTADO;
        }
        if ((size_t) leidos != sizeof solicitud) {
            fprintf(stderr, PREFIJO " Solicitud truncada (%zd de %zu bytes).\n",
                    leidos, sizeof solicitud);
            return SESION_CLIENTE_DESCONECTADO;
        }
        if (solicitud.magia != MAGIA_PROTOCOLO) {
            fprintf(stderr, PREFIJO " Mensaje con número mágico inválido.\n");
            (void) enviar_error(fd_respuesta, "Mensaje no reconocido por el protocolo");
            return SESION_CLIENTE_DESCONECTADO;
        }
        solicitud.texto[TAM_TEXTO - 1] = '\0'; // blindaje ante datos corruptos 

        describir_solicitud(&solicitud, descripcion, sizeof descripcion);
        printf(PREFIJO " Solicitud del cliente PID %d: %s\n",
               (int) solicitud.pid_cliente, descripcion);

        switch (atender_solicitud(fd_respuesta, &solicitud, ruta_csv)) {
        case SOLICITUD_ATENDIDA:
            break;
        case SOLICITUD_DESCONECTAR:
            printf(PREFIJO " El cliente PID %d se desconectó.\n",
                   (int) solicitud.pid_cliente);
            return SESION_CLIENTE_DESCONECTADO;
        case SOLICITUD_DETENER:
            printf(PREFIJO " Orden de terminación recibida del cliente PID %d.\n",
                   (int) solicitud.pid_cliente);
            return SESION_DETENER_SERVIDOR;
        case SOLICITUD_FALLO_ENVIO:
            return terminar_servidor ? SESION_DETENER_SERVIDOR
                                     : SESION_CLIENTE_DESCONECTADO;
        }
    }
    return SESION_DETENER_SERVIDOR;
}

// ------------------ main ------------------
int main(int argc, char *argv[])
{
    const char *ruta_csv = (argc > 1) ? argv[1] : RUTA_CSV_POR_DEFECTO;
    int codigo_salida = EXIT_SUCCESS;

    if (argc > 2) {
        fprintf(stderr, "Uso: %s [archivo.csv]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (instalar_manejadores_terminacion(manejar_senal_terminacion) == -1) {
        return EXIT_FAILURE;
    }
    // Comprobar el dataset antes de crear recursos IPC
    if (access(ruta_csv, R_OK) == -1) {
        fprintf(stderr, PREFIJO " No se puede leer '%s': %s\n", ruta_csv,
                strerror(errno));
        return EXIT_FAILURE;
    }
    if (crear_fifo(FIFO_SOLICITUD) == -1 || crear_fifo(FIFO_RESPUESTA) == -1) {
        eliminar_recursos_fifo();
        return EXIT_FAILURE;
    }

    printf(PREFIJO " PID=%d PPID=%d | dataset: %s\n", (int) getpid(),
           (int) getppid(), ruta_csv);
    printf(PREFIJO " Ctrl+C o 'kill -TERM %d' para terminar ordenadamente.\n",
           (int) getpid());

    while (!terminar_servidor) {
        int fd_solicitud;
        int fd_respuesta;
        resultado_sesion_t resultado;

        printf(PREFIJO " Esperando cliente (open bloqueante de %s)...\n",
               FIFO_SOLICITUD);
        fflush(stdout);

        // Se bloquea hasta que un cliente abra la FIFO para escribir
        fd_solicitud = open(FIFO_SOLICITUD, O_RDONLY);
        if (fd_solicitud == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror(PREFIJO " open(FIFO de solicitudes)");
            codigo_salida = EXIT_FAILURE;
            break;
        }

        // Se bloquea hasta que el cliente abra la FIFO de respuestas para leer
        fd_respuesta = open(FIFO_RESPUESTA, O_WRONLY);
        if (fd_respuesta == -1) {
            int error_guardado = errno;
            cerrar_descriptor(fd_solicitud, "FIFO de solicitudes");
            if (error_guardado == EINTR) {
                continue;
            }
            fprintf(stderr, PREFIJO " open(FIFO de respuestas): %s\n",
                    strerror(error_guardado));
            codigo_salida = EXIT_FAILURE;
            break;
        }

        printf(PREFIJO " Cliente conectado (fd solicitudes=%d, fd respuestas=%d)\n",
               fd_solicitud, fd_respuesta);

        resultado = atender_sesion(fd_solicitud, fd_respuesta, ruta_csv);

        cerrar_descriptor(fd_respuesta, "FIFO de respuestas");
        cerrar_descriptor(fd_solicitud, "FIFO de solicitudes");

        if (resultado == SESION_DETENER_SERVIDOR) {
            break;
        }
    }

    if (terminar_servidor) {
        printf("\n" PREFIJO " Señal de terminación recibida.\n");
    }
    eliminar_recursos_fifo();
    printf(PREFIJO " Recursos liberados. Fin.\n");
    return codigo_salida;
}
