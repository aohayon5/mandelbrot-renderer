#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <math.h>
#include <time.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint32_t size;
    uint32_t reserved1;
    uint32_t offset;
} BmpFileHeader;

typedef struct {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bits;
    uint32_t compression;
    uint32_t imagesize;
    int32_t xresolution;
    int32_t yresolution;
    uint32_t ncolors;
    uint32_t importantcolors;
} BmpInfoHeader;
#pragma pack(pop)

//Req 3: Shares data across threads
typedef struct {
    double x;       
    double y;        
    int row;       
    int col;        
    int is_valid;   
} EngineDataShare;

//Thread struct
typedef struct {
    int id;     
    int type; //0 -> column prod, 1 -> engine cosumer, 2 -> writer thread for barrier
} ThreadStuff;

int img_dim;
int engines;
double UL_X, UL_Y;
double mandel_dim;

//Req 3: array of structs to share data per row
EngineDataShare* DataShare_struct_array;
//Req 5c: mutex for every engine to ensure column threads take turns
pthread_mutex_t* engine_mutexes;   
//Solve wake up on engine thread
pthread_cond_t* engine_c;      
pthread_cond_t* work_column_c;
//Req 5d: semaphore for one engine write to row data at a time
sem_t row_write_sem;           
//Req 5E and 5G: barrier functionality
pthread_barrier_t row_barrier;
pthread_mutex_t writer_mutex;     // Mutex for writer thread
pthread_cond_t row_barrier_c;    // Condition variable for writer thread
//Req 4: store row image data
int* current_row_data; 
uint8_t* current_row_rgb;
int current_row = 0; 
int all_work_submitted = 0;
int* pixels_per_row;
int* row_completed;
int* row_written;
int total_points_processed = 0;
FILE* output_file;

//BMP file setup
void init_bmp_file(FILE* file, int width, int height) {
    BmpFileHeader fileHeader = {0x4D42, sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + width * height * 3, 0, sizeof(BmpFileHeader) + sizeof(BmpInfoHeader)};
    BmpInfoHeader infoHeader = {sizeof(BmpInfoHeader), width, height, 1, 24, 0, width * height * 3, 0, 0, 0, 0};
    fwrite(&fileHeader, sizeof(BmpFileHeader), 1, file);
    fwrite(&infoHeader, sizeof(BmpInfoHeader), 1, file);
}


//private method to do the actual writing to BMP
void write_row_to_bmp(FILE* file, uint8_t* row_data, int width, int row_index) {
    long position = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + (img_dim - 1 - row_index) * width * 3;
    fseek(file, position, SEEK_SET);
    fwrite(row_data, 3, width, file);
    fflush(file);
}

//Req 3: private method to calculate mandelbrot, calls correspond to engine work
int calculate_mandelbrot(double x, double y) {
    const int MAX_ITERATIONS = 255;  // As specified in the requirements
    double real = x;
    double imag = y;
    double real_squared = real * real;
    double imag_squared = imag * imag;
    int iterations = 0;
    while (real_squared + imag_squared <= 4.0 && iterations < MAX_ITERATIONS) {
        double next_imag = 2 * real * imag + y;
        real = real_squared - imag_squared + x;
        imag = next_imag;
        real_squared = real * real;
        imag_squared = imag * imag;
        iterations++;
    }
    return 255 - iterations;
}

//Col thread (producer) calculates coordinates
void* column_thread(void* arg) {
    ThreadStuff* args = (ThreadStuff*)arg;
    int column = args->id;
    //Req 5b: col does calc on comlpex plane
    double x = UL_X + (column * mandel_dim) / img_dim;
    for (int row = 0; row < img_dim; row++) {
        //Req 5E: uses barrier to prevent next row starting until prev completed, skips first row that should start immediately
        if (row > 0) {
            pthread_barrier_wait(&row_barrier);
        }
        //Req 5b: col does calc on comlpex plane
        double y = UL_Y + (row * mandel_dim) / img_dim;
        //Req 5A: random engine
        int engine_id = rand() % engines;
        //generate struct
        EngineDataShare item;
        item.x = x;
        item.y = y;
        item.row = row;
        item.col = column;
        item.is_valid = 1;
        //Req 5c: mutex for turns write
        pthread_mutex_lock(&engine_mutexes[engine_id]);
        //Use condition variables to wake up engine threads when the prev struct work is completed
        while (DataShare_struct_array[engine_id].is_valid) {
            pthread_cond_wait(&work_column_c[engine_id], &engine_mutexes[engine_id]);
        }
        DataShare_struct_array[engine_id] = item;
        //Completed so more work possible to take on
        pthread_cond_signal(&engine_c[engine_id]);
        pthread_mutex_unlock(&engine_mutexes[engine_id]);
    }
    //Will crash without this free on large input, need to give back thread space?
    free(args);
    return NULL;
}

//Req 3: engine thread (consumer) consume work given by column thread to actually calculate the mandelbrot
void* engine_thread(void* arg) {
    ThreadStuff* args = (ThreadStuff*)arg;
    int engine_id = args->id;
    while (!all_work_submitted || DataShare_struct_array[engine_id].is_valid) {
        //legal mutex, just checking again reduntanly that there is work need for some reason.
        pthread_mutex_lock(&engine_mutexes[engine_id]);
        //Use condition variables to wake up engine threads when the prev struct work is completed
        while (!DataShare_struct_array[engine_id].is_valid && !all_work_submitted) {
            pthread_cond_wait(&engine_c[engine_id], &engine_mutexes[engine_id]);
        }
        //Make sure actually has something to do
        if (!DataShare_struct_array[engine_id].is_valid) {
            if (all_work_submitted) {
                pthread_mutex_unlock(&engine_mutexes[engine_id]);
                break;
            }
            pthread_mutex_unlock(&engine_mutexes[engine_id]);
            continue;
        }
        //retrieve work and free slot for producer
        EngineDataShare item = DataShare_struct_array[engine_id];
        DataShare_struct_array[engine_id].is_valid = 0;
        pthread_cond_signal(&work_column_c[engine_id]);
        pthread_mutex_unlock(&engine_mutexes[engine_id]);
        //Req 3: do consumer job and actually calculate
        int value = calculate_mandelbrot(item.x, item.y);
        
        //Req 5D: Using a semaphore to make sure only one engine writes to row at a time
        sem_wait(&row_write_sem);
        current_row_data[item.col] = value;
        total_points_processed++;
        pixels_per_row[item.row]++;
        
        //If row is complete write it to the bmp and then we can move on
        if (pixels_per_row[item.row] == img_dim) {
            row_completed[item.row] = 1;
            //Need Bmp style color formatting
            for (int col = 0; col < img_dim; col++) {
                current_row_rgb[col*3] = current_row_data[col];   
                current_row_rgb[col*3 + 1] = current_row_data[col];
                current_row_rgb[col*3 + 2] = current_row_data[col]; 
            }
            write_row_to_bmp(output_file, current_row_rgb, img_dim, item.row);
            row_written[item.row] = 1;
            //Need an extra mutex again to work with my extra thread that will reach barrier once all of this is completed,
            //once signaled it can also hit the barrier, and then we can move on to the next row
            pthread_mutex_lock(&writer_mutex);
            pthread_cond_signal(&row_barrier_c);
            pthread_mutex_unlock(&writer_mutex);
        }
        sem_post(&row_write_sem);
    }
    free(args);
    return NULL;
}

//Req 5G: extra thread to coordinate work reaches barrier last
void* writer_thread_function(void* arg) {
    ThreadStuff* args = (ThreadStuff*)arg;
    for (int row = 0; row < img_dim; row++) {
        //Check row completed?
        pthread_mutex_lock(&writer_mutex);
        while (!row_written[row]) {
            pthread_cond_wait(&row_barrier_c, &writer_mutex);
        }
        pthread_mutex_unlock(&writer_mutex);
        //skip last row, because the rest of the threads are self terminating once everyuthing is done and barrier would never release
        if (row < img_dim - 1) {
            pthread_barrier_wait(&row_barrier);
        }
    }
    free(args);
    return NULL;
}

int main(int argc, char *argv[]) {
    img_dim = atoi(argv[1]);
    engines = atoi(argv[2]);
    UL_X = atof(argv[3]);
    UL_Y = atof(argv[4]);
    mandel_dim = atof(argv[5]);
    
    //Req 5A: initiate randomizer
    srand(time(NULL));

    //malloc array of structs
    DataShare_struct_array = (EngineDataShare*)malloc(engines * sizeof(EngineDataShare));
    for (int i = 0; i < engines; i++) {
        DataShare_struct_array[i].is_valid = 0;  // Initialize as invalid
    }
    
    //malloc condition variable related space
    engine_mutexes = (pthread_mutex_t*)malloc(engines * sizeof(pthread_mutex_t));
    engine_c = (pthread_cond_t*)malloc(engines * sizeof(pthread_cond_t));
    work_column_c = (pthread_cond_t*)malloc(engines * sizeof(pthread_cond_t));
    //create threads and cond vars
    for (int i = 0; i < engines; i++) {
        pthread_mutex_init(&engine_mutexes[i], NULL);
        pthread_cond_init(&engine_c[i], NULL);
        pthread_cond_init(&work_column_c[i], NULL);
    }
    //Req 5d: semaphore for row writes turns
    sem_init(&row_write_sem, 0, 1);
    //Req 5 E and G thread barrier
    pthread_barrier_init(&row_barrier, NULL, img_dim + 1);
    
    //Extra mutex just used for synchronization with extra thread and initiate that to
    pthread_mutex_init(&writer_mutex, NULL);
    pthread_cond_init(&row_barrier_c, NULL);
    
    //Req 4: row data array malloc management
    current_row_data = (int*)malloc(img_dim * sizeof(int));
    current_row_rgb = (uint8_t*)malloc(img_dim * 3 * sizeof(uint8_t));
    pixels_per_row = (int*)calloc(img_dim, sizeof(int));
    row_completed = (int*)calloc(img_dim, sizeof(int));
    row_written = (int*)calloc(img_dim, sizeof(int)); 
    memset(current_row_data, 0, img_dim * sizeof(int));

    output_file = fopen("mandeloutput.bmp", "wb");
    if (!output_file) {
        perror("Opening file error");
        return 1;
    }
    init_bmp_file(output_file, img_dim, img_dim);
    //create threads space
    pthread_t* column_threads = (pthread_t*)malloc(img_dim * sizeof(pthread_t));
    pthread_t* engine_threads = (pthread_t*)malloc(engines * sizeof(pthread_t));
    pthread_t writer_thread;
    //The extra thread for writer_thread_function and the extra 1 for barrier
    ThreadStuff* writer_args = (ThreadStuff*)malloc(sizeof(ThreadStuff));
    writer_args->id = 0;
    writer_args->type = 2;
    pthread_create(&writer_thread, NULL, writer_thread_function, writer_args);
    //engine thread create
    for (int i = 0; i < engines; i++) {
        ThreadStuff* args = (ThreadStuff*)malloc(sizeof(ThreadStuff));
        args->id = i;
        args->type = 1;
        pthread_create(&engine_threads[i], NULL, engine_thread, args);
    }
    //column producer thread creation
    for (int i = 0; i < img_dim; i++) {
        ThreadStuff* args = (ThreadStuff*)malloc(sizeof(ThreadStuff));
        args->id = i;
        args->type = 0; 
        pthread_create(&column_threads[i], NULL, column_thread, args);
    }
    
    //check when nothing to produce
    for (int i = 0; i < img_dim; i++) {
        pthread_join(column_threads[i], NULL);
    }
    all_work_submitted = 1;
    //No more work so engines finish their work and then terminate also
    for (int i = 0; i < engines; i++) {
        pthread_mutex_lock(&engine_mutexes[i]);
        pthread_cond_signal(&engine_c[i]);
        pthread_mutex_unlock(&engine_mutexes[i]);
    }
    //Join engine threads
    for (int i = 0; i < engines; i++) {
        pthread_join(engine_threads[i], NULL);
    }
    //Extra thread joins also ends
    pthread_join(writer_thread, NULL);
    fclose(output_file);

    //clear up work space
    for (int i = 0; i < engines; i++) {
        pthread_mutex_destroy(&engine_mutexes[i]);
        pthread_cond_destroy(&engine_c[i]);
        pthread_cond_destroy(&work_column_c[i]);
    }
    free(DataShare_struct_array);
    free(engine_mutexes);
    free(engine_c);
    free(work_column_c);
    sem_destroy(&row_write_sem);
    pthread_barrier_destroy(&row_barrier);
    pthread_mutex_destroy(&writer_mutex);
    pthread_cond_destroy(&row_barrier_c);
    free(current_row_data);
    free(current_row_rgb);
    free(pixels_per_row);
    free(row_completed);
    free(row_written);
    free(column_threads);
    free(engine_threads);
    return 0;
}