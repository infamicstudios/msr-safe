// Copyright 2011-2023 Lawrence Livermore National Security, LLC and other
// msr-safe Project Developers. See the top-level COPYRIGHT file for
// details.
//
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>      // printf(3)
#include <assert.h>     // assert(3)
#include <fcntl.h>      // open(2)
#include <unistd.h>     // write(2), pwrite(2), pread(2)
#include <string.h>     // strlen(3), memset(3)
#include <stdint.h>     // uint8_t
#include <inttypes.h>   // PRIu8
#include <stdlib.h>     // exit(3)
#include <sys/ioctl.h>  // ioctl(2)
#include <sys/time.h>   // itimer

#include <time.h>   // itimer
#include <signal.h>     // sig_atomic_t
#include "../msr_safe.h"   // batch data structs

/*
* Polls energy data from the cpu saving it as
* a time series allowing it to be analysed etc.
* Writing this for now to run on lupine not sure 
* if other systems use the same MSR's
*/

#define MSR_RAPL_POWER_UNIT 0x606   // Haswell
#define MSR_PKG_ENERGY_STATUS 0x611 // Haswell
#define MSR_PKG_POWER_INFO 0x614    // Haswell

#define INTERVAL_SEC 1

#define MAX_CPUs 64

char const *const allowlist = "0x611 0xFFFFFFFFFFFFFFFF\n";

static uint8_t const nCPUs = 32;

void measure_energy_batch(struct msr_batch_op* res);
void process_data(size_t col, struct msr_batch_op (*data)[nCPUs][col]);

void _measure_energy_time_series(size_t time, double interval_sec);

struct energy_data {
    size_t *data;       // circular buffer
    size_t size;
    time_t *timestamps; // Timestamps of measurement
    size_t start;       // index of oldest
    size_t end;         // index of most recent
};

volatile sig_atomic_t flag = 0;
size_t measurement_count = 0;
size_t col = 0;

//struct energy_data *cpu_data[MAX_CPUs];
struct energy_data **cpu_data;


void set_allowlist()
{
    int fd = open("/dev/cpu/msr_allowlist", O_WRONLY);
    assert(-1 != fd);
    ssize_t nbytes = write(fd, allowlist, strlen(allowlist));
    assert(strlen(allowlist) == nbytes);
    close(fd);
}


void create_buffers(size_t size) {
    cpu_data = malloc(nCPUs * sizeof(*cpu_data));
    assert(cpu_data != NULL);

    for (uint8_t i = 0; i < nCPUs; i++) {
        cpu_data[i] = malloc(sizeof(*cpu_data[i]));
        assert(cpu_data[i] != NULL);
        cpu_data[i]->data = malloc(size * sizeof(*cpu_data[i]->data));
        cpu_data[i]->timestamps = malloc(size * sizeof(*cpu_data[i]->timestamps));
        assert(cpu_data[i]->data != NULL);
        cpu_data[i]->size = size;
        cpu_data[i]->start = 0;
        cpu_data[i]->end = 0;
    }
}

void free_buffers() {
    for (uint8_t i = 0; i < nCPUs; i++) {
        free(cpu_data[i]->data);
        free(cpu_data[i]);
    }
    free(cpu_data);
}

//Subprocess stuff
void handler(int signum) {
  flag = 1;
}

void setup_timer() {
  struct itimerval timer;
  timer.it_interval.tv_sec = INTERVAL_SEC;
  timer.it_interval.tv_usec = 0;
  timer.it_value = timer.it_interval;
  setitimer(ITIMER_REAL, &timer, NULL);
}

// Update a CPU's circular buf.
void update_data(uint8_t cpu, size_t energy) {
    //TODO: These timestamps aren't working quite right
    time_t raw_time;
    time(&raw_time);

    cpu_data[cpu]->data[cpu_data[cpu]->end] = energy;
    cpu_data[cpu]->timestamps[cpu_data[cpu]->end] = raw_time;
    cpu_data[cpu]->end = (cpu_data[cpu]->end + 1) % cpu_data[cpu]->size;
    if (cpu_data[cpu]->end == cpu_data[cpu]->start) {
        // Overwrite oldest data pont.
        cpu_data[cpu]->start = (cpu_data[cpu]->start + 1) % cpu_data[cpu]->size;
    }
}

int get_max_columns() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int max_columns = w.ws_col / 6;  // Dividing by 6 due to 6 digits of energy data and 1 space
    max_columns -= 18;  // reserve space for "Average delta: " and its value
    return max_columns > 0 ? max_columns : 1;  // Ensure it's always at least 1
}

void print_graph() {
    printf("\033[H\033[J");  // clear screen

    int max_columns = get_max_columns();

    // Print data and calculate average delta change
    for (uint8_t i = 0; i < nCPUs; i++) {
        double delta_sum = 0;
        int delta_count = 0;

        printf("CPU %2u: ", i);

        size_t size = (cpu_data[i]->end - cpu_data[i]->start + cpu_data[i]->size) % cpu_data[i]->size;
        size_t index = (cpu_data[i]->end + cpu_data[i]->size - (size < max_columns ? size : max_columns)) % cpu_data[i]->size;
        size_t count = 0;
        size_t previous_measurement = cpu_data[i]->data[index];

        while (index != cpu_data[i]->end) {
            printf("%6luJ ", cpu_data[i]->data[index]);
            if (count > 0) {  // Skip the first measurement, as there's no previous measurement to compare it to
                delta_sum += abs((int)cpu_data[i]->data[index] - (int)previous_measurement);
                delta_count++;
            }
            previous_measurement = cpu_data[i]->data[index];
            index = (index + 1) % cpu_data[i]->size;
            count++;
        }

        while (count < max_columns) {
            printf("%6lu ", 0LU);
            count++;
        }

        double avg_delta_change = delta_count > 0 ? delta_sum / delta_count : 0;
        printf(" | Average delta: %.2fJ\n", avg_delta_change);
    }

    // Print timestamps
    printf("Time (s): ");
    size_t size = (cpu_data[0]->end - cpu_data[0]->start + cpu_data[0]->size) % cpu_data[0]->size;
    size_t index = (cpu_data[0]->end + cpu_data[0]->size - (size < max_columns ? size : max_columns)) % cpu_data[0]->size;
    size_t count = 0;

    while (index != cpu_data[0]->end) {
        printf("%6luS ", cpu_data[0]->timestamps[index] % 60);  // print seconds
        index = (index + 1) % cpu_data[0]->size;
        count++;
    }

    while (count < max_columns) {
        printf("%6s ", "--");
        count++;
    }

    putchar('\n');
}

void measure_energy_time_series(size_t time, double interval_sec)
{
  struct msr_batch_op res[nCPUs];
  size_t col = (size_t)(time/interval_sec);

  _measure_energy_time_series(time, interval_sec);

  create_buffers(col);

  signal(SIGALRM, handler);
  setup_timer();

  while(1){
    if (flag == 1) {
      measure_energy_batch(res);
      for(uint8_t j = 0; j < nCPUs; j++){
        update_data(j, res[j].msrdata);
      }
      print_graph();
      measurement_count++;
      flag = 0;
    }
    if(measurement_count >= col) {
      break;
    }
  }
  free_buffers();
}


void _measure_energy_time_series(size_t time, double interval_sec)
{
  fprintf(stdout, "============== Measuring Energy Status as a Time Series =============\n");
  fprintf(stdout, "Recording data for %"PRIu64" Seconds with an interval of %f\n", 
                  time, interval_sec);
  fprintf(stdout, "# Data points %f\n", (size_t)time/interval_sec);
  sleep(1);
}

void measure_energy_batch(struct msr_batch_op *res)
{
    struct msr_batch_array rbatch, wbatch;
    struct msr_batch_op r_ops[nCPUs], w_ops[nCPUs];
    int fd, rc;
    double enrg_stat_unit = 1 << 5; // Doc Specs default.. 

    fd = open("/dev/cpu/msr_batch", O_RDONLY);
    assert(-1 != fd);

    for (uint8_t i = 0; i < nCPUs; i++)
    {
        r_ops[i].cpu = w_ops[i].cpu = i;
        r_ops[i].isrdmsr = 1;
        w_ops[i].isrdmsr = 0;
        r_ops[i].msr = w_ops[i].msr = MSR_PKG_ENERGY_STATUS;
        w_ops[i].msrdata = 0;
    }
    rbatch.numops = wbatch.numops = nCPUs;
    rbatch.ops = r_ops;
    wbatch.ops = w_ops;

    rc = ioctl(fd, X86_IOC_MSR_BATCH, &wbatch);

    rc = ioctl(fd, X86_IOC_MSR_BATCH, &rbatch);

    for (uint8_t i = 0; i < nCPUs; i++)
    {
          r_ops[i].msrdata = (r_ops[i].msrdata * enrg_stat_unit) / 1e6;
          res[i].msrdata = r_ops[i].msrdata;
    }
}

int main() {
  set_allowlist();
  measure_energy_time_series(600, 1.0);
  return 0;
}



/*================ UNUSED ==========
void measure_power_draw:()
{
    int fd[nCPUs], rc;
    char filename[255];
    uint64_t enrg_stat_raw[nCPUs];
    uint64_t enrg_draw_J[nCPUs]; // TODO: Can probably do without some of these
    double enrg_stat_unit = 1 << 5; // Doc specs ~= pow(2, 5)

    memset(enrg_stat_raw, 0, sizeof(uint64_t)*nCPUs);
    memset(enrg_draw_J, 0, sizeof(uint64_t)*nCPUs);

    // Open each of the msr_safe devices (one per CPU)
    for (uint8_t i = 0; i < nCPUs; i++)
    {
        rc = snprintf(filename, 254, "/dev/cpu/%"PRIu8"/msr_safe", i);
        assert(-1 != rc);
        fd[i] = open(filename, O_RDWR);
        assert(-1 != fd[i]);
    }

    // Write 0 to each ENRG status register
    for (uint8_t i = 0; i < nCPUs; i++)
    {
        rc = pwrite(fd[i], &enrg_stat_raw[i], sizeof(uint64_t), MSR_PKG_ENERGY_STATUS);
    }

  
    // Read each RAPL_POWER_UNIT && PKG_ENERGY_STATUS 
    for (uint8_t i = 0; i < nCPUs; i++)
    {
        pread(fd[i], &enrg_stat_raw[i], sizeof(uint64_t), MSR_PKG_ENERGY_STATUS); 
        enrg_draw_J[i] = (enrg_stat_raw[i] * enrg_stat_unit) / 1e6;
    }

    // Show results
    printf("Energy status from first to last read?:\n");
    fprintf(stdout, "CPU  | Energy Draw J\n");
    for (uint8_t i = 0; i < nCPUs; i++)
    {
        fprintf(stdout, "%4"PRIu8" | %"PRIu64" J\n", i, enrg_draw_J[i]);
    }
}
*/

