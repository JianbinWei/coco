#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <errno.h>

#include "coco.h"

#include "coco_generics.c"
#include "coco_utilities.c"
#include "coco_problem.c"
#include "coco_strdup.c"
#include "observer_bbob2009.c"

static int bbob2009_raisedOptValWarning;

static const size_t bbob2009_nbpts_nbevals = 20;
static const size_t bbob2009_nbpts_fval = 5;
static size_t bbob2009_current_dim = 0;
static long bbob2009_current_funId = 0;
static long bbob2009_infoFile_firstInstance = 0;
char bbob2009_infoFile_firstInstance_char[3];
/* a possible solution: have a list of dims that are already in the file, if the ones we're about to log
 * is != bbob2009_current_dim and the funId is currend_funId, create a new .info file with as suffix the
 * number of the first instance */
static const int bbob2009_number_of_dimensions = 6;
static size_t bbob2009_dimensions_in_current_infoFile[6] = { 0, 0, 0, 0, 0, 0 }; /* TODO should use SUITE_BBOB2009_NUMBER_OF_DIMENSIONS */

/* The current_... mechanism fails if several problems are open.
 * For the time being this should lead to an error.
 *
 * A possible solution: bbob2009_logger_is_open becomes a reference
 * counter and as long as another logger is open, always a new info
 * file is generated.
 */
static int bbob2009_logger_is_open = 0; /* this could become lock-list of .info files */

/* TODO: add possibility of adding a prefix to the index files */

typedef struct {
  coco_observer_t *observer;
  int is_initialized;
  char *path; /* relative path to the data folder. Simply the Algname*/
  const char * alg_name; /* the alg name, for now, temporarily the same as the path. Now in the observer */
  FILE *index_file; /* index file */
  FILE *fdata_file; /* function value aligned data file */
  FILE *tdata_file; /* number of function evaluations aligned data file */
  FILE *rdata_file; /* restart info data file */
  double f_trigger; /* next upper bound on the fvalue to trigger a log in the .dat file*/
  long t_trigger; /* next lower bound on nb fun evals to trigger a log in the .tdat file*/
  int idx_f_trigger; /* allows to track the index i in logging target = {10**(i/bbob2009_nbpts_fval), i \in Z} */
  int idx_t_trigger; /* allows to track the index i in logging nbevals = {int(10**(i/bbob2009_nbpts_nbevals)), i \in Z} */
  int idx_tdim_trigger; /* allows to track the index i in logging nbevals = {dim * 10**i, i \in Z} */
  long number_of_evaluations;
  double best_fvalue;
  double last_fvalue;
  short written_last_eval; /* allows writing the the data of the final fun eval in the .tdat file if not already written by the t_trigger*/
  double *best_solution;
  /* The following are to only pass data as a parameter in the free function. The
   * interface should probably be the same for all free functions so passing the
   * problem as a second parameter is not an option even though we need info
   * form it.*/
  int function_id; /*TODO: consider changing name*/
  long instance_id;
  size_t number_of_variables;
  double optimal_fvalue;
} logger_bbob2009_t;

static const char *bbob2009_file_header_str = "%% function evaluation | "
    "noise-free fitness - Fopt (%13.12e) | "
    "best noise-free fitness - Fopt | "
    "measured fitness | "
    "best measured fitness | "
    "x1 | "
    "x2...\n";

static void logger_bbob2009_update_f_trigger(logger_bbob2009_t *logger, double fvalue) {
  /* "jump" directly to the next closest (but larger) target to the
   * current fvalue from the initial target
   */

  if (fvalue - logger->optimal_fvalue <= 0.) {
    logger->f_trigger = -DBL_MAX;
  } else {
    if (logger->idx_f_trigger == INT_MAX) { /* first time*/
      logger->idx_f_trigger = (int) (ceil(log10(fvalue - logger->optimal_fvalue))
          * (double) (long) bbob2009_nbpts_fval);
    } else { /* We only call this function when we reach the current f_trigger*/
      logger->idx_f_trigger--;
    }
    logger->f_trigger = pow(10, logger->idx_f_trigger * 1.0 / (double) (long) bbob2009_nbpts_fval);
    while (fvalue - logger->optimal_fvalue <= logger->f_trigger) {
      logger->idx_f_trigger--;
      logger->f_trigger = pow(10, logger->idx_f_trigger * 1.0 / (double) (long) bbob2009_nbpts_fval);
    }
  }
}

static void logger_bbob2009_update_t_trigger(logger_bbob2009_t *logger, size_t number_of_variables) {
  while (logger->number_of_evaluations
      >= floor(pow(10, (double) logger->idx_t_trigger / (double) (long) bbob2009_nbpts_nbevals)))
    logger->idx_t_trigger++;

  while (logger->number_of_evaluations
      >= (double) (long) number_of_variables * pow(10, (double) logger->idx_tdim_trigger))
    logger->idx_tdim_trigger++;

  logger->t_trigger = (long) coco_min_double(
      floor(pow(10, (double) logger->idx_t_trigger / (double) (long) bbob2009_nbpts_nbevals)),
      (double) (long) number_of_variables * pow(10, (double) logger->idx_tdim_trigger));
}

/**
 * adds a formated line to a data file
 */
static void logger_bbob2009_write_data(FILE *target_file,
                                       long number_of_evaluations,
                                       double fvalue,
                                       double best_fvalue,
                                       double best_value,
                                       const double *x,
                                       size_t number_of_variables) {
  /* for some reason, it's %.0f in the old code instead of the 10.9e
   * in the documentation
   */
  fprintf(target_file, "%ld %+10.9e %+10.9e %+10.9e %+10.9e", number_of_evaluations, fvalue - best_value,
      best_fvalue - best_value, fvalue, best_fvalue);
  if (number_of_variables < 22) {
    size_t i;
    for (i = 0; i < number_of_variables; i++) {
      fprintf(target_file, " %+5.4e", x[i]);
    }
  }
  fprintf(target_file, "\n");
}

/**
 * Error when trying to create the file "path"
 */
static void logger_bbob2009_error_io(FILE *path, int errnum) {
  char *buf;
  const char *error_format = "Error opening file: %s\n ";
  /* "bbob2009_logger_prepare() failed to open log file '%s'.";*/
  size_t buffer_size = (size_t) snprintf(NULL, 0, error_format, path); /* to silence warning */
  buf = (char *) coco_allocate_memory(buffer_size);
  snprintf(buf, buffer_size, error_format, strerror(errnum), path);
  coco_error(buf);
  coco_free_memory(buf);
}

/**
 * Creates the data files or simply opens it
 */

/*
 calling sequence:
 _bbob2009_logger_open_dataFile(&(logger->fdata_file), logger->path, dataFile_path,
 ".dat");
 */

static void logger_bbob2009_open_dataFile(FILE **target_file,
                                          const char *path,
                                          const char *dataFile_path,
                                          const char *file_extension) {
  char file_path[COCO_PATH_MAX] = { 0 };
  char relative_filePath[COCO_PATH_MAX] = { 0 };
  int errnum;
  strncpy(relative_filePath, dataFile_path,
  COCO_PATH_MAX - strlen(relative_filePath) - 1);
  strncat(relative_filePath, file_extension,
  COCO_PATH_MAX - strlen(relative_filePath) - 1);
  coco_join_path(file_path, sizeof(file_path), path, relative_filePath, NULL);
  if (*target_file == NULL) {
    *target_file = fopen(file_path, "a+");
    errnum = errno;
    if (*target_file == NULL) {
      logger_bbob2009_error_io(*target_file, errnum);
    }
  }
}

/*
 static void private_logger_bbob2009_open_dataFile(FILE **target_file, const char *path,
 const char *dataFile_path, const char *file_extension) {
 char file_path[COCO_PATH_MAX] = { 0 };
 char relative_filePath[COCO_PATH_MAX] = { 0 };
 int errnum;
 strncpy(relative_filePath, dataFile_path,
 COCO_PATH_MAX - strlen(relative_filePath) - 1);
 strncat(relative_filePath, file_extension,
 COCO_PATH_MAX - strlen(relative_filePath) - 1);
 coco_join_path(file_path, sizeof(file_path), path, relative_filePath, NULL);
 if (*target_file == NULL) {
 *target_file = fopen(file_path, "a+");
 errnum = errno;
 if (*target_file == NULL) {
 _bbob2009_logger_error_io(*target_file, errnum);
 }
 }
 }
 */

/**
 * Creates the index file fileName_prefix+problem_id+file_extension in
 * folde_path
 */
static void logger_bbob2009_openIndexFile(logger_bbob2009_t *logger,
                                          const char *folder_path,
                                          const char *indexFile_prefix,
                                          const char *function_id,
                                          const char *dataFile_path) {
  /* to add the instance number TODO: this should be done outside to avoid redoing this for the .*dat files */
  char used_dataFile_path[COCO_PATH_MAX] = { 0 };
  int errnum, newLine; /* newLine is at 1 if we need a new line in the info file */
  char function_id_char[3]; /* TODO: consider adding them to logger */
  char file_name[COCO_PATH_MAX] = { 0 };
  char file_path[COCO_PATH_MAX] = { 0 };
  FILE **target_file;
  FILE *tmp_file;
  strncpy(used_dataFile_path, dataFile_path, COCO_PATH_MAX - strlen(used_dataFile_path) - 1);
  if (bbob2009_infoFile_firstInstance == 0) {
    bbob2009_infoFile_firstInstance = logger->instance_id;
  }
  sprintf(function_id_char, "%d", logger->function_id);
  sprintf(bbob2009_infoFile_firstInstance_char, "%ld", bbob2009_infoFile_firstInstance);
  target_file = &(logger->index_file);
  tmp_file = NULL; /* to check whether the file already exists. Don't want to use target_file */
  strncpy(file_name, indexFile_prefix, COCO_PATH_MAX - strlen(file_name) - 1);
  strncat(file_name, "_f", COCO_PATH_MAX - strlen(file_name) - 1);
  strncat(file_name, function_id_char, COCO_PATH_MAX - strlen(file_name) - 1);
  strncat(file_name, "_i", COCO_PATH_MAX - strlen(file_name) - 1);
  strncat(file_name, bbob2009_infoFile_firstInstance_char, COCO_PATH_MAX - strlen(file_name) - 1);
  strncat(file_name, ".info", COCO_PATH_MAX - strlen(file_name) - 1);
  coco_join_path(file_path, sizeof(file_path), folder_path, file_name, NULL);
  if (*target_file == NULL) {
    tmp_file = fopen(file_path, "r"); /* to check for existence */
    if ((tmp_file) &&(bbob2009_current_dim == logger->number_of_variables)
        && (bbob2009_current_funId == logger->function_id)) { /* new instance of current funId and current dim */
      newLine = 0;
      *target_file = fopen(file_path, "a+");
      if (*target_file == NULL) {
        errnum = errno;
        logger_bbob2009_error_io(*target_file, errnum);
      }
      fclose(tmp_file);
    } else { /* either file doesn't exist (new funId) or new Dim */
      /* check that the dim was not already present earlier in the file, if so, create a new info file */
      if (bbob2009_current_dim != logger->number_of_variables) {
        int i, j;
        for (i = 0;
            i < bbob2009_number_of_dimensions && bbob2009_dimensions_in_current_infoFile[i] != 0
                && bbob2009_dimensions_in_current_infoFile[i] != logger->number_of_variables; i++) {
          ; /* checks whether dimension already present in the current infoFile */
        }
        if (i < bbob2009_number_of_dimensions && bbob2009_dimensions_in_current_infoFile[i] == 0) {
          /* new dimension seen for the first time */
          bbob2009_dimensions_in_current_infoFile[i] = logger->number_of_variables;
          newLine = 1;
        } else {
          if (i < bbob2009_number_of_dimensions) { /* dimension already present, need to create a new file */
            newLine = 0;
            file_path[strlen(file_path) - strlen(bbob2009_infoFile_firstInstance_char) - 7] = 0; /* truncate the instance part */
            bbob2009_infoFile_firstInstance = logger->instance_id;
            sprintf(bbob2009_infoFile_firstInstance_char, "%ld", bbob2009_infoFile_firstInstance);
            strncat(file_path, "_i", COCO_PATH_MAX - strlen(file_name) - 1);
            strncat(file_path, bbob2009_infoFile_firstInstance_char, COCO_PATH_MAX - strlen(file_name) - 1);
            strncat(file_path, ".info", COCO_PATH_MAX - strlen(file_name) - 1);
          } else {/*we have all dimensions*/
            newLine = 1;
          }
          for (j = 0; j < bbob2009_number_of_dimensions; j++) { /* new info file, reinitialize list of dims */
            bbob2009_dimensions_in_current_infoFile[j] = 0;
          }
          bbob2009_dimensions_in_current_infoFile[i] = logger->number_of_variables;
        }
      }
      *target_file = fopen(file_path, "a+"); /* in any case, we append */
      if (*target_file == NULL) {
        errnum = errno;
        logger_bbob2009_error_io(*target_file, errnum);
      }
      if (tmp_file) { /* File already exists, new dim so just a new line. ALso, close the tmp_file */
        if (newLine) {
          fprintf(*target_file, "\n");
        }

        fclose(tmp_file);
      }

      fprintf(*target_file, "funcId = %d, DIM = %lu, Precision = %.3e, algId = '%s'\n",
          (int) strtol(function_id, NULL, 10), logger->number_of_variables, pow(10, -8), logger->alg_name);
      fprintf(*target_file, "%%\n");
      strncat(used_dataFile_path, "_i", COCO_PATH_MAX - strlen(used_dataFile_path) - 1);
      strncat(used_dataFile_path, bbob2009_infoFile_firstInstance_char,
      COCO_PATH_MAX - strlen(used_dataFile_path) - 1);
      fprintf(*target_file, "%s.dat", used_dataFile_path); /* dataFile_path does not have the extension */
      bbob2009_current_dim = logger->number_of_variables;
      bbob2009_current_funId = logger->function_id;
    }
  }
}

/**
 * Generates the different files and folder needed by the logger to store the
 * data if these don't already exist
 */
static void logger_bbob2009_initialize(logger_bbob2009_t *logger, coco_problem_t *inner_problem) {
  /*
   Creates/opens the data and index files
   */
  char dataFile_path[COCO_PATH_MAX] = { 0 }; /* relative path to the .dat file from where the .info file is */
  char folder_path[COCO_PATH_MAX] = { 0 };
  char tmpc_funId[3]; /* serves to extract the function id as a char *. There should be a better way of doing this! */
  char tmpc_dim[3]; /* serves to extract the dimension as a char *. There should be a better way of doing this! */
  char indexFile_prefix[10] = "bbobexp"; /* TODO (minor): make the prefix bbobexp a parameter that the user can modify */
  assert(logger != NULL);
  assert(inner_problem != NULL);
  assert(inner_problem->problem_id != NULL);

  sprintf(tmpc_funId, "%d", coco_problem_get_suite_dep_function_id(inner_problem));
  sprintf(tmpc_dim, "%lu", (unsigned long) inner_problem->number_of_variables);

  /* prepare paths and names */
  strncpy(dataFile_path, "data_f", COCO_PATH_MAX);
  strncat(dataFile_path, tmpc_funId,
  COCO_PATH_MAX - strlen(dataFile_path) - 1);
  coco_join_path(folder_path, sizeof(folder_path), logger->path, dataFile_path,
  NULL);
  coco_create_path(folder_path);
  strncat(dataFile_path, "/bbobexp_f",
  COCO_PATH_MAX - strlen(dataFile_path) - 1);
  strncat(dataFile_path, tmpc_funId,
  COCO_PATH_MAX - strlen(dataFile_path) - 1);
  strncat(dataFile_path, "_DIM", COCO_PATH_MAX - strlen(dataFile_path) - 1);
  strncat(dataFile_path, tmpc_dim, COCO_PATH_MAX - strlen(dataFile_path) - 1);

  /* index/info file */
  logger_bbob2009_openIndexFile(logger, logger->path, indexFile_prefix, tmpc_funId, dataFile_path);
  fprintf(logger->index_file, ", %ld", coco_problem_get_suite_dep_instance_id(inner_problem));
  /* data files */
  /* TODO: definitely improvable but works for now */
  strncat(dataFile_path, "_i", COCO_PATH_MAX - strlen(dataFile_path) - 1);
  strncat(dataFile_path, bbob2009_infoFile_firstInstance_char,
  COCO_PATH_MAX - strlen(dataFile_path) - 1);
  logger_bbob2009_open_dataFile(&(logger->fdata_file), logger->path, dataFile_path, ".dat");
  fprintf(logger->fdata_file, bbob2009_file_header_str, logger->optimal_fvalue);

  logger_bbob2009_open_dataFile(&(logger->tdata_file), logger->path, dataFile_path, ".tdat");
  fprintf(logger->tdata_file, bbob2009_file_header_str, logger->optimal_fvalue);

  logger_bbob2009_open_dataFile(&(logger->rdata_file), logger->path, dataFile_path, ".rdat");
  fprintf(logger->rdata_file, bbob2009_file_header_str, logger->optimal_fvalue);
  /* TODO: manage duplicate filenames by either using numbers or raising an error */
  /* The coco_create_unique_path() function is available now! */
  logger->is_initialized = 1;
}

/**
 * Layer added to the transformed-problem evaluate_function by the logger
 */
static void logger_bbob2009_evaluate(coco_problem_t *self, const double *x, double *y) {
  logger_bbob2009_t *logger = coco_transformed_get_data(self);
  coco_problem_t * inner_problem = coco_transformed_get_inner_problem(self);

  if (!logger->is_initialized) {
    logger_bbob2009_initialize(logger, inner_problem);
  }
  if ((coco_log_level >= COCO_INFO) && logger->number_of_evaluations == 0) {
    if (inner_problem->suite_dep_index >= 0) {
      coco_info("%4ld: ", inner_problem->suite_dep_index);
    }
    coco_info("on problem %s ... ", coco_problem_get_id(inner_problem));
  }
  coco_evaluate_function(inner_problem, x, y);
  logger->last_fvalue = y[0];
  logger->written_last_eval = 0;
  if (logger->number_of_evaluations == 0 || y[0] < logger->best_fvalue) {
    size_t i;
    logger->best_fvalue = y[0];
    for (i = 0; i < self->number_of_variables; i++)
      logger->best_solution[i] = x[i];
  }
  logger->number_of_evaluations++;

  /* Add sanity check for optimal f value */
  /* assert(y[0] >= logger->optimal_fvalue); */
  if (!bbob2009_raisedOptValWarning && y[0] < logger->optimal_fvalue) {
    coco_warning("Observed fitness is smaller than supposed optimal fitness.");
    bbob2009_raisedOptValWarning = 1;
  }

  /* Add a line in the .dat file for each logging target reached. */
  if (y[0] - logger->optimal_fvalue <= logger->f_trigger) {

    logger_bbob2009_write_data(logger->fdata_file, logger->number_of_evaluations, y[0], logger->best_fvalue,
        logger->optimal_fvalue, x, self->number_of_variables);
    logger_bbob2009_update_f_trigger(logger, y[0]);
  }

  /* Add a line in the .tdat file each time an fevals trigger is reached. */
  if (logger->number_of_evaluations >= logger->t_trigger) {
    logger->written_last_eval = 1;
    logger_bbob2009_write_data(logger->tdata_file, logger->number_of_evaluations, y[0], logger->best_fvalue,
        logger->optimal_fvalue, x, self->number_of_variables);
    logger_bbob2009_update_t_trigger(logger, self->number_of_variables);
  }

  /* Flush output so that impatient users can see progress. */
  fflush(logger->fdata_file);
}

/**
 * Also serves as a finalize run method so. Must be called at the end
 * of Each run to correctly fill the index file
 *
 * TODO: make sure it is called at the end of each run or move the
 * writing into files to another function
 */
static void logger_bbob2009_free(void *stuff) {
  /* TODO: do all the "non simply freeing" stuff in another function
   * that can have problem as input
   */
  logger_bbob2009_t *logger = stuff;

  if ((coco_log_level >= COCO_INFO) && logger && logger->number_of_evaluations > 0) {
    coco_info("best f=%e after %ld fevals (done observing)\n", logger->best_fvalue, logger->number_of_evaluations);
  }
  if (logger->alg_name != NULL) {
    coco_free_memory((void*) logger->alg_name);
    logger->alg_name = NULL;
  }

  if (logger->path != NULL) {
    coco_free_memory(logger->path);
    logger->path = NULL;
  }
  if (logger->index_file != NULL) {
    fprintf(logger->index_file, ":%ld|%.1e", logger->number_of_evaluations,
        logger->best_fvalue - logger->optimal_fvalue);
    fclose(logger->index_file);
    logger->index_file = NULL;
  }
  if (logger->fdata_file != NULL) {
    fclose(logger->fdata_file);
    logger->fdata_file = NULL;
  }
  if (logger->tdata_file != NULL) {
    /* TODO: make sure it handles restarts well. i.e., it writes
     * at the end of a single run, not all the runs on a given
     * instance. Maybe start with forcing it to generate a new
     * "instance" of problem for each restart in the beginning
     */
    if (!logger->written_last_eval) {
      logger_bbob2009_write_data(logger->tdata_file, logger->number_of_evaluations, logger->last_fvalue,
          logger->best_fvalue, logger->optimal_fvalue, logger->best_solution, logger->number_of_variables);
    }
    fclose(logger->tdata_file);
    logger->tdata_file = NULL;
  }

  if (logger->rdata_file != NULL) {
    fclose(logger->rdata_file);
    logger->rdata_file = NULL;
  }

  if (logger->best_solution != NULL) {
    coco_free_memory(logger->best_solution);
    logger->best_solution = NULL;
  }
  bbob2009_logger_is_open = 0;
}

/* Wassim: to be removed */
static coco_problem_t *depreciated_logger_bbob2009(coco_problem_t *inner_problem, const char *alg_name) {
  logger_bbob2009_t *logger;
  coco_problem_t *self;
  logger = coco_allocate_memory(sizeof(*logger));
  logger->alg_name = coco_strdup(alg_name);
  if (bbob2009_logger_is_open)
    coco_error("The current bbob2009_logger (observer) must be closed before a new one is opened");
  /* This is the name of the folder which happens to be the algName */
  logger->path = coco_strdup(alg_name);
  logger->index_file = NULL;
  logger->fdata_file = NULL;
  logger->tdata_file = NULL;
  logger->rdata_file = NULL;
  logger->number_of_variables = inner_problem->number_of_variables;
  if (inner_problem->best_value == NULL) {
    /* coco_error("Optimal f value must be defined for each problem in order for the logger to work properly"); */
    /* Setting the value to 0 results in the assertion y>=optimal_fvalue being susceptible to failure */
    coco_warning("undefined optimal f value. Set to 0");
    logger->optimal_fvalue = 0;
  } else {
    logger->optimal_fvalue = *(inner_problem->best_value);
  }
  bbob2009_raisedOptValWarning = 0;

  logger->idx_f_trigger = INT_MAX;
  logger->idx_t_trigger = 0;
  logger->idx_tdim_trigger = 0;
  logger->f_trigger = DBL_MAX;
  logger->t_trigger = 0;
  logger->number_of_evaluations = 0;
  logger->best_solution = coco_allocate_vector(inner_problem->number_of_variables);
  /* TODO: the following inits are just to be in the safe side and
   * should eventually be removed. Some fields of the bbob2009_logger struct
   * might be useless
   */
  logger->function_id = coco_problem_get_suite_dep_function_id(inner_problem);
  logger->instance_id = coco_problem_get_suite_dep_instance_id(inner_problem);
  logger->written_last_eval = 1;
  logger->last_fvalue = DBL_MAX;
  logger->is_initialized = 0;

  self = coco_transformed_allocate(inner_problem, logger, logger_bbob2009_free);
  self->evaluate_function = logger_bbob2009_evaluate;
  bbob2009_logger_is_open = 1;
  return self;
}


static coco_problem_t *logger_bbob2009(coco_observer_t *observer, coco_problem_t *problem) {
  const char *alg_name=observer->output_folder;
  
  logger_bbob2009_t *logger;
  /*logger_bbob2009_t *logger;*/
  coco_problem_t *self;
  
  logger = coco_allocate_memory(sizeof(*logger));
  logger->observer = observer;
  /*logger = coco_allocate_memory(sizeof(*logger));*/
  logger->alg_name = coco_strdup(alg_name);
  

  if (problem->number_of_objectives != 1) {
    coco_warning("logger_toy(): The toy logger shouldn't be used to log a problem with %d objectives", problem->number_of_objectives);
  }
  
  if (bbob2009_logger_is_open)
    coco_error("The current bbob2009_logger (observer) must be closed before a new one is opened");
  /* This is the name of the folder which happens to be the algName */
  logger->path = coco_strdup(alg_name);
  logger->index_file = NULL;
  logger->fdata_file = NULL;
  logger->tdata_file = NULL;
  logger->rdata_file = NULL;
  logger->number_of_variables = problem->number_of_variables;
  if (problem->best_value == NULL) {
    /* coco_error("Optimal f value must be defined for each problem in order for the logger to work properly"); */
    /* Setting the value to 0 results in the assertion y>=optimal_fvalue being susceptible to failure */
    coco_warning("undefined optimal f value. Set to 0");
    logger->optimal_fvalue = 0;
  } else {
    logger->optimal_fvalue = *(problem->best_value);
  }
  bbob2009_raisedOptValWarning = 0;
  
  logger->idx_f_trigger = INT_MAX;
  logger->idx_t_trigger = 0;
  logger->idx_tdim_trigger = 0;
  logger->f_trigger = DBL_MAX;
  logger->t_trigger = 0;
  logger->number_of_evaluations = 0;
  logger->best_solution = coco_allocate_vector(problem->number_of_variables);
  /* TODO: the following inits are just to be in the safe side and
   * should eventually be removed. Some fields of the bbob2009_logger struct
   * might be useless
   */
  logger->function_id = coco_problem_get_suite_dep_function_id(problem);
  logger->instance_id = coco_problem_get_suite_dep_instance_id(problem);
  logger->written_last_eval = 1;
  logger->last_fvalue = DBL_MAX;
  logger->is_initialized = 0;
  
  self = coco_transformed_allocate(problem, logger, logger_bbob2009_free);
  self->evaluate_function = logger_bbob2009_evaluate;
  bbob2009_logger_is_open = 1;
  return self;
}

