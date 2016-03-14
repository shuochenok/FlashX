/**
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Disa Mhembere (disa)
 *
 * This file is part of FlashGraph.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "matrix/kmeans_coordinator.h"

int main (int argc, char* argv []) {

    if (argc < 6) {
        fprintf(stderr, "usage: ./test_kmeans_coordinator nthreads "
                "nrow ncol k datafile\n");
        exit(911);
    }

    unsigned nrow = atoi(argv[2]);
    unsigned ncol = atoi(argv[3]);
    unsigned k = atoi(argv[4]);
    std::string fn = argv[5];
    constexpr unsigned max_iters = 50;
    unsigned nnodes = numa_num_task_nodes();
    unsigned nthreads = atoi(argv[1]);
    std::string init = "random";

    kmeans_coordinator::ptr kc = kmeans_coordinator::create(fn,
            nrow, ncol, k, max_iters, nnodes, nthreads, init);
    kc->run_kmeans();
    return(EXIT_SUCCESS);
}
