/*
 servidor_shm.c — servidor con memoria compartida POSIX y semaforos.

 Crea los recursos IPC /shm_<GRUPO> y /sem_{turno,solicitud,respuesta,consumido}_<GRUPO> y los elimina
 al terminar. Protocolo productor-consumidor por bloques:
 
 Solo el cliente con el turno escribe la solicitud y solo el servidor escribe registros[]; el protocolo
 de semaforos evita condiciones de carrera in espera activa.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun/csv.h"
#include "../comun/protocolo.h"
#include "../comun/semaforos.h"
#include "../comun/utilidades.h"

#define PREFIJO "[servidor_shm]"

static volatile sig_atomic_t terminar_servidor = 0;

typedef struct {
    int fd_memoria;
    region_compartida_t *region;
    sem_t *sem_turno;
    sem_t *sem_solicitud;
    sem_t *sem_respuesta;
    sem_t *sem_consumido;
} recursos_shm_t;

typedef struct {
    recursos_shm_t *recursos;
    pid_t pid_cliente; /* para detectar si el cliente murio */
    int cliente_perdido; /* 1 si el cliente desaparecio */
} contexto_bloques_t;

typedef enum {
    SOLICITUD_ATENDIDA,
    SOLICITUD_DETENER,
    SOLICITUD_CLIENTE_PERDIDO,
    SOLICITUD_INTERRUMPIDA
} resultado_solicitud_t;

static void manejar_senal_terminacion(int numero_senal)
{
    (void) numero_senal;
    terminar_servidor = 1;
}

/*
Si hay una region previa cuyo servidor sigue vivo, devuelve su PID, si el
propietario murio, la region es basura y se puede eliminar
*/
static pid_t servidor_previo_activo(void)
{
    struct stat informacion;
    pid_t pid_activo = 0;
    int fd = shm_open(SHM_NOMBRE, O_RDONLY, 0);

    if (fd == -1) {
        return 0;
    }
    if (fstat(fd, &informacion) == 0 &&
        (size_t) informacion.st_size >= sizeof(region_compartida_t)) {
        region_compartida_t *region = mmap(NULL, sizeof *region, PROT_READ,
                                           MAP_SHARED, fd, 0);
        if (region != MAP_FAILED) {
            if (region->magia == MAGIA_PROTOCOLO && region->servidor_activo &&
                region->pid_servidor != (int32_t) getpid() &&
                proceso_existe((pid_t) region->pid_servidor)) {
                pid_activo = (pid_t) region->pid_servidor;
            }
            if (munmap(region, sizeof *region) == -1) {
                perror(PREFIJO " munmap(region previa)");
            }
        }
    }
    cerrar_descriptor(fd, "memoria compartida previa");
    return pid_activo;
}

static void eliminar_nombres_ipc(int informar)
{
    const char *semaforos[] = { SEM_TURNO, SEM_SOLICITUD, SEM_RESPUESTA, SEM_CONSUMIDO };
    size_t i;

    if (shm_unlink(SHM_NOMBRE) == 0) {
        if (informar) {
            printf(PREFIJO " shm_unlink(%s)\n", SHM_NOMBRE);
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, PREFIJO " shm_unlink(%s): %s\n", SHM_NOMBRE, strerror(errno));
    }

    for (i = 0; i < sizeof semaforos / sizeof semaforos[0]; i++) {
        if (sem_unlink(semaforos[i]) == 0) {
            if (informar) {
                printf(PREFIJO " sem_unlink(%s)\n", semaforos[i]);
            }
        } else if (errno != ENOENT) {
            fprintf(stderr, PREFIJO " sem_unlink(%s): %s\n", semaforos[i],
                    strerror(errno));
        }
    }
}

static sem_t *crear_semaforo(const char *nombre, unsigned int valor_inicial)
{
    sem_t *semaforo = sem_open(nombre, O_CREAT | O_EXCL, 0600, valor_inicial);

    if (semaforo == SEM_FAILED) {
        fprintf(stderr, PREFIJO " sem_open(%s): %s\n", nombre, strerror(errno));
        return NULL;
    }
    return semaforo;
}

static void cerrar_semaforo(sem_t **semaforo, const char *nombre)
{
    if (*semaforo != NULL) {
        if (sem_close(*semaforo) == -1) {
            fprintf(stderr, PREFIJO " sem_close(%s): %s\n", nombre, strerror(errno));
        }
        *semaforo = NULL;
    }
}

static int crear_recursos(recursos_shm_t *recursos)
{
    void *direccion;

    recursos->fd_memoria = shm_open(SHM_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (recursos->fd_memoria == -1) {
        perror(PREFIJO " shm_open(" SHM_NOMBRE ")");
        return -1;
    }
    /* Un objeto recien creado mide 0 bytes: hay que darle tamano */
    if (ftruncate(recursos->fd_memoria, (off_t) sizeof(region_compartida_t)) == -1) {
        perror(PREFIJO " ftruncate");
        return -1;
    }
    direccion = mmap(NULL, sizeof(region_compartida_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, recursos->fd_memoria, 0);
    if (direccion == MAP_FAILED) {
        perror(PREFIJO " mmap");
        return -1;
    }
    recursos->region = direccion;
    memset(recursos->region, 0, sizeof *recursos->region);
    recursos->region->magia = MAGIA_PROTOCOLO;
    recursos->region->pid_servidor = (int32_t) getpid();
    recursos->region->estado = EST_LIBRE;

    if ((recursos->sem_turno = crear_semaforo(SEM_TURNO, 1)) == NULL ||
        (recursos->sem_solicitud = crear_semaforo(SEM_SOLICITUD, 0)) == NULL ||
        (recursos->sem_respuesta = crear_semaforo(SEM_RESPUESTA, 0)) == NULL ||
        (recursos->sem_consumido = crear_semaforo(SEM_CONSUMIDO, 0)) == NULL) {
        return -1;
    }

    recursos->region->servidor_activo = 1;
    return 0;
}

/*
munmap, close, sem_close y unlink de nombres. Es seguro llamarla con
recursos parcialmente creados
*/
static void liberar_recursos(recursos_shm_t *recursos)
{
    if (recursos->region != NULL) {
        recursos->region->servidor_activo = 0;
        if (munmap(recursos->region, sizeof *recursos->region) == -1) {
            perror(PREFIJO " munmap");
        } else {
            printf(PREFIJO " munmap de la region\n");
        }
        recursos->region = NULL;
    }
    if (recursos->fd_memoria != -1) {
        cerrar_descriptor(recursos->fd_memoria, "memoria compartida");
        recursos->fd_memoria = -1;
    }
    cerrar_semaforo(&recursos->sem_turno, SEM_TURNO);
    cerrar_semaforo(&recursos->sem_solicitud, SEM_SOLICITUD);
    cerrar_semaforo(&recursos->sem_respuesta, SEM_RESPUESTA);
    cerrar_semaforo(&recursos->sem_consumido, SEM_CONSUMIDO);
    eliminar_nombres_ipc(1);
}

/*
Avisa al cliente de que el bloque esta completo y duerme hasta que lo haya
copiado. Vigila que el cliente siga vivo
*/
static int publicar_y_esperar(contexto_bloques_t *contexto)
{
    recursos_shm_t *recursos = contexto->recursos;
    resultado_espera_t espera;

    recursos->region->numero_bloque++;
    /* sem_post actua como barrera: lo escrito antes queda visible al cliente */
    if (sem_post(recursos->sem_respuesta) == -1) {
        perror(PREFIJO " sem_post(respuesta)");
        return -1;
    }

    espera = esperar_semaforo_vigilado(recursos->sem_consumido, contexto->pid_cliente,
                                       &terminar_servidor, SEGUNDOS_VIGILANCIA);
    switch (espera) {
    case ESPERA_OK:
        return 0;
    case ESPERA_PROCESO_AUSENTE:
        fprintf(stderr, PREFIJO " El cliente PID %d termino sin consumir la "
                "respuesta; se aborta la consulta.\n", (int) contexto->pid_cliente);
        contexto->cliente_perdido = 1;
        return -1;
    case ESPERA_INTERRUMPIDA:
    case ESPERA_ERROR:
    default:
        return -1;
    }
}

/*
Callback de buscar_en_csv: si el bloque esta lleno, lo publica (EST_BLOQUE)
y espera a que el cliente lo consuma antes de reutilizar el bufer
*/
static int agregar_registro_a_bloque(const registro_t *registro, void *contexto_opaco)
{
    contexto_bloques_t *contexto = contexto_opaco;
    region_compartida_t *region = contexto->recursos->region;

    if (terminar_servidor) {
        return -1;
    }
    if (region->num_registros == REGISTROS_POR_BLOQUE) {
        region->estado = EST_BLOQUE;
        if (publicar_y_esperar(contexto) == -1) {
            return -1;
        }
        region->num_registros = 0;
    }
    region->registros[region->num_registros++] = *registro;
    return 0;
}

static resultado_solicitud_t publicar_final(contexto_bloques_t *contexto,
                                            int32_t estado,
                                            resultado_solicitud_t si_exito)
{
    contexto->recursos->region->estado = estado;
    if (publicar_y_esperar(contexto) == -1) {
        return contexto->cliente_perdido ? SOLICITUD_CLIENTE_PERDIDO
                                         : SOLICITUD_INTERRUMPIDA;
    }
    return si_exito;
}

static resultado_solicitud_t atender_solicitud(recursos_shm_t *recursos,
                                               const solicitud_t *solicitud,
                                               const char *ruta_csv)
{
    region_compartida_t *region = recursos->region;
    contexto_bloques_t contexto = { recursos, (pid_t) solicitud->pid_cliente, 0 };
    char mensaje[TAM_MENSAJE_ERROR];
    resumen_t resumen;
    int resultado;

    region->num_registros = 0;
    region->numero_bloque = 0;
    region->mensaje_error[0] = '\0';
    memset(&region->resumen, 0, sizeof region->resumen);

    if (solicitud->magia != MAGIA_PROTOCOLO) {
        copiar_cadena(region->mensaje_error, sizeof region->mensaje_error,
                      "Solicitud no reconocida por el protocolo");
        return publicar_final(&contexto, EST_ERROR, SOLICITUD_ATENDIDA);
    }

    if (solicitud->operacion == OP_DESCONECTAR) {
        return publicar_final(&contexto, EST_ADIOS, SOLICITUD_ATENDIDA);
    }
    if (solicitud->operacion == OP_TERMINAR) {
        resultado_solicitud_t r = publicar_final(&contexto, EST_ADIOS, SOLICITUD_DETENER);
        return r == SOLICITUD_CLIENTE_PERDIDO ? SOLICITUD_DETENER : r;
    }

    if (validar_solicitud(solicitud, mensaje, sizeof mensaje) == -1) {
        fprintf(stderr, PREFIJO " Solicitud rechazada: %s\n", mensaje);
        copiar_cadena(region->mensaje_error, sizeof region->mensaje_error, mensaje);
        return publicar_final(&contexto, EST_ERROR, SOLICITUD_ATENDIDA);
    }

    resultado = buscar_en_csv(ruta_csv, solicitud, agregar_registro_a_bloque,
                              &contexto, &resumen, mensaje, sizeof mensaje);
    if (resultado == -1) {
        fprintf(stderr, PREFIJO " %s\n", mensaje);
        region->num_registros = 0;
        copiar_cadena(region->mensaje_error, sizeof region->mensaje_error, mensaje);
        return publicar_final(&contexto, EST_ERROR, SOLICITUD_ATENDIDA);
    }
    if (resultado == -2) {
        return contexto.cliente_perdido ? SOLICITUD_CLIENTE_PERDIDO
                                        : SOLICITUD_INTERRUMPIDA;
    }

    printf(PREFIJO " %d coincidencias, %d enviadas en %d bloque(s), %lld lineas "
           "leidas, %.2f ms\n", (int) resumen.coincidencias, (int) resumen.enviados,
           (int) region->numero_bloque + 1, (long long) resumen.lineas_leidas,
           resumen.milisegundos);

    region->resumen = resumen;
    return publicar_final(&contexto, EST_FIN, SOLICITUD_ATENDIDA);
}

int main(int argc, char *argv[])
{
    const char *ruta_csv = (argc > 1) ? argv[1] : RUTA_CSV_POR_DEFECTO;
    recursos_shm_t recursos = { -1, NULL, NULL, NULL, NULL, NULL };
    int codigo_salida = EXIT_SUCCESS;
    pid_t pid_previo;

    if (argc > 2) {
        fprintf(stderr, "Uso: %s [archivo.csv]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (instalar_manejadores_terminacion(manejar_senal_terminacion) == -1) {
        return EXIT_FAILURE;
    }
    if (access(ruta_csv, R_OK) == -1) {
        fprintf(stderr, PREFIJO " No se puede leer '%s': %s\n", ruta_csv,
                strerror(errno));
        return EXIT_FAILURE;
    }

    /* No arrancar si ya hay otro servidor vivo usando los mismos nombres */
    pid_previo = servidor_previo_activo();
    if (pid_previo > 0) {
        fprintf(stderr, PREFIJO " Ya hay un servidor_shm activo (PID %d).\n",
                (int) pid_previo);
        return EXIT_FAILURE;
    }
    /* Limpiar restos de una ejecucion anterior que termino mal */
    eliminar_nombres_ipc(0);

    if (crear_recursos(&recursos) == -1) {
        liberar_recursos(&recursos);
        return EXIT_FAILURE;
    }

    printf(PREFIJO " PID=%d PPID=%d | dataset: %s\n", (int) getpid(),
           (int) getppid(), ruta_csv);
    printf(PREFIJO " Region %s de %zu bytes (fd=%d) mapeada en %p\n", SHM_NOMBRE,
           sizeof(region_compartida_t), recursos.fd_memoria, (void *) recursos.region);
    printf(PREFIJO " Ctrl+C o 'kill -TERM %d' para terminar ordenadamente.\n",
           (int) getpid());

    while (!terminar_servidor) {
        solicitud_t solicitud;
        resultado_solicitud_t resultado;
        resultado_espera_t espera;
        char descripcion[256];

        printf(PREFIJO " Esperando solicitud (sem_wait %s)...\n", SEM_SOLICITUD);
        fflush(stdout);

        espera = esperar_semaforo_vigilado(recursos.sem_solicitud, 0,
                                           &terminar_servidor, SEGUNDOS_VIGILANCIA);
        if (espera == ESPERA_INTERRUMPIDA) {
            break;
        }
        if (espera != ESPERA_OK) {
            codigo_salida = EXIT_FAILURE;
            break;
        }

        /* Copia local: a partir de aqui no se depende de la region */
        solicitud = recursos.region->solicitud;
        solicitud.texto[TAM_TEXTO - 1] = '\0';

        describir_solicitud(&solicitud, descripcion, sizeof descripcion);
        printf(PREFIJO " Solicitud del cliente PID %d: %s\n",
               (int) solicitud.pid_cliente, descripcion);

        resultado = atender_solicitud(&recursos, &solicitud, ruta_csv);

        if (resultado == SOLICITUD_CLIENTE_PERDIDO) {
            vaciar_semaforo(recursos.sem_respuesta);
            vaciar_semaforo(recursos.sem_consumido);
        }

        /* El servidor devuelve el turno (una vez por solicitud) */
        recursos.region->estado = EST_LIBRE;
        if (sem_post(recursos.sem_turno) == -1) {
            perror(PREFIJO " sem_post(turno)");
        }

        if (resultado == SOLICITUD_DETENER) {
            printf(PREFIJO " Orden de terminacion recibida del cliente PID %d.\n",
                   (int) solicitud.pid_cliente);
            break;
        }
        if (resultado == SOLICITUD_INTERRUMPIDA) {
            break;
        }
    }

    if (terminar_servidor) {
        printf("\n" PREFIJO " Senal de terminacion recibida.\n");
    }
    liberar_recursos(&recursos);
    printf(PREFIJO " Recursos IPC liberados. Fin.\n");
    return codigo_salida;
}