#include <strings.h>    /* strncasecmp */
#include <apr_errno.h>
#include <apr_thread_proc.h>
#include <threadproc.h>
#include <apr_strings.h>
#include "config.h"
#include "flood_farmer.h"

#include "flood_farm.h"

extern apr_file_t *local_stdout;
extern apr_file_t *local_stderr;

struct farm_t {
    const char *name;
    int n_farmers;
    apr_thread_t **farmers; /* pointer to array of threads */
};
typedef struct farm_t farm_t;

struct farmer_worker_info_t {
    const char *farmer_name;
    config_t *config;
    apr_thread_t **thr;
};
typedef struct farmer_worker_info_t farmer_worker_info_t;

/**
 * Worker function that is assigned to a thread. Each worker is
 * called a farmer in our system.
 */
void * APR_THREAD_FUNC farmer_worker(void *data)
{
    apr_status_t stat;
    apr_pool_t *pool;
    farmer_worker_info_t * info = (farmer_worker_info_t *)data;
    pool = (*info->thr)->cntxt;

    /* should we create a subpool here? */
    apr_file_printf(local_stdout, "Starting farmer_worker thread '%s'.\n",
                    info->farmer_name);

    if ((stat = run_farmer(info->config, info->farmer_name,
                           pool)) != APR_SUCCESS) {
        char buf[256];
        apr_strerror(stat, (char*) &buf, 256);
        apr_file_printf(local_stderr, "Error running farmer '%s': %s.\n",
                        info->farmer_name, (char*) &buf);
        /* just die for now, later try to return status */
    }

    return NULL;
}

apr_status_t run_farm(config_t *config, const char *farm_name, apr_pool_t *pool)
{
    apr_status_t stat, child_stat;
    int usefarmer_count, i;
    char *xml_farm, **usefarmer_names;
    struct apr_xml_elem *e, *root_elem, *farm_elem;
    farm_t *farm;
    farmer_worker_info_t *infovec;

    xml_farm = apr_pstrdup(pool, XML_FARM);

    /* get the root config node */
    if ((stat = retrieve_root_xml_elem(&root_elem, config)) != APR_SUCCESS) {
        return stat;
    }

    /* get farmer node from config */
    if ((stat = retrieve_xml_elem_with_childmatch(
             &farm_elem, root_elem,
             xml_farm, XML_FARM_NAME, farm_name)) != APR_SUCCESS)
        return stat;

    /* count the number of "usefarmer" children */
    usefarmer_count = count_xml_elem_child(farm_elem, XML_FARM_USEFARMER);

    /* create each of the children and put their names in an array */
    usefarmer_names = apr_palloc(pool, sizeof(char*) * (usefarmer_count + 1));
    /* set the sentinel (no need for pcalloc()) */
    usefarmer_names[usefarmer_count] = NULL; 
    i = 0;
    for (e = farm_elem->first_child; e; e = e->next) {
        if (strncasecmp(e->name, XML_FARM_USEFARMER, FLOOD_STRLEN_MAX) == 0) {
            usefarmer_names[i++] = apr_pstrdup(pool, 
                                               e->first_cdata.first->text);
        }
    }

    /* create the farm object */
    farm = apr_pcalloc(pool, sizeof(farm_t));
    farm->name = apr_pstrdup(pool, farm_name);
    farm->n_farmers = usefarmer_count;
    farm->farmers = apr_pcalloc(pool, 
                                sizeof(apr_thread_t*) * (usefarmer_count + 1));

    infovec = apr_pcalloc(pool, sizeof(farmer_worker_info_t) * usefarmer_count);

    /* for each of my farmers, start them */
    for (i = 0; i < usefarmer_count; i++) {
        infovec[i].farmer_name = usefarmer_names[i];
        infovec[i].config = config;
        infovec[i].thr = &farm->farmers[i];
        if ((stat = apr_thread_create(&farm->farmers[i],
                                      NULL,
                                      farmer_worker,
                                      (void*) &infovec[i],
                                      pool)) != APR_SUCCESS) {
            /* error, perhaps shutdown other threads then exit? */
            return stat;
        }
    }

    for (i = 0; i < usefarmer_count; i++) {
        if ((stat = apr_thread_join(&child_stat, farm->farmers[i])) != APR_SUCCESS) {
            apr_file_printf(local_stderr, "Error joining farmer thread '%d' ('%s').\n",
                            i, usefarmer_names[i]);
            return stat;
        } else {
            apr_file_printf(local_stdout, "Farmer '%d' ('%s') completed successfully.\n",
                            i, usefarmer_names[i]);
        }
    }

    return APR_SUCCESS;
}

