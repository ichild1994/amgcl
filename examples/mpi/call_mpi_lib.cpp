#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>

#include <boost/scope_exit.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/numeric.hpp>

#include <amgcl_mpi.h>

#include "domain_partition.hpp"

double STDCALL constant_deflation(int, long, void*) {
    return 1;
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);
    BOOST_SCOPE_EXIT(void) {
        MPI_Finalize();
    } BOOST_SCOPE_EXIT_END

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (rank == 0)
        std::cout << "World size: " << size << std::endl;

    const long n  = argc > 1 ? atoi(argv[1]) : 1024;
    const long n2 = n * n;

    // Partition
    boost::array<long, 2> lo = { {0, 0} };
    boost::array<long, 2> hi = { {n - 1, n - 1} };

    domain_partition<2> part(lo, hi, size);
    long chunk = part.size( rank );

    std::vector<long> domain(size + 1);
    MPI_Allgather(&chunk, 1, MPI_LONG, &domain[1], 1, MPI_LONG, MPI_COMM_WORLD);
    boost::partial_sum(domain, domain.begin());

    long chunk_start = domain[rank];
    long chunk_end   = domain[rank + 1];

    std::vector<long> renum(n2);
    for(long j = 0, idx = 0; j < n; ++j) {
        for(long i = 0; i < n; ++i, ++idx) {
            boost::array<long, 2> p = {{i, j}};
            std::pair<int,long> v = part.index(p);
            renum[idx] = domain[v.first] + v.second;
        }
    }

    // Assemble
    std::vector<long>   ptr;
    std::vector<long>   col;
    std::vector<double> val;
    std::vector<double> rhs;

    ptr.reserve(chunk + 1);
    col.reserve(chunk * 5);
    val.reserve(chunk * 5);
    rhs.reserve(chunk);

    ptr.push_back(0);

    const double hinv = (n - 1);
    const double h2i  = (n - 1) * (n - 1);
    for(long j = 0, idx = 0; j < n; ++j) {
        for(long i = 0; i < n; ++i, ++idx) {
            if (renum[idx] < chunk_start || renum[idx] >= chunk_end) continue;

            if (j > 0)  {
                col.push_back(renum[idx - n]);
                val.push_back(-h2i);
            }

            if (i > 0) {
                col.push_back(renum[idx - 1]);
                val.push_back(-h2i - hinv);
            }

            col.push_back(renum[idx]);
            val.push_back(4 * h2i + hinv);

            if (i + 1 < n) {
                col.push_back(renum[idx + 1]);
                val.push_back(-h2i);
            }

            if (j + 1 < n) {
                col.push_back(renum[idx + n]);
                val.push_back(-h2i);
            }

            rhs.push_back(1);
            ptr.push_back( col.size() );
        }
    }

    // Setup
    amgclHandle prm    = amgcl_params_create();
    amgclHandle solver = amgcl_mpi_create(
            amgclBackendBuiltin,
            amgclCoarseningSmoothedAggregation,
            amgclRelaxationSPAI0,
            amgclSolverBiCGStabL,
#ifdef HAVE_PASTIX
            amgclDirectSolverPastix,
#else
            amgclDirectSolverSkylineLU,
#endif
            prm, MPI_COMM_WORLD,
            chunk, ptr.data(), col.data(), val.data(),
            1, constant_deflation, NULL
            );

    // Solve
    std::vector<double> x(chunk, 0);
    amgcl_mpi_solve(solver, rhs.data(), x.data());

    // Clean up
    amgcl_mpi_destroy(solver);
    amgcl_params_destroy(prm);

    if (n <= 4096) {
        if (rank == 0) {
            std::vector<double> X(n2);
            boost::copy(x, X.begin());

            for(int i = 1; i < size; ++i)
                MPI_Recv(&X[domain[i]], domain[i+1] - domain[i], MPI_DOUBLE, i, 42, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            std::ofstream f("out.dat", std::ios::binary);
            int m = n2;
            f.write((char*)&m, sizeof(int));
            for(long i = 0; i < n2; ++i)
                f.write((char*)&X[renum[i]], sizeof(double));
        } else {
            MPI_Send(x.data(), chunk, MPI_DOUBLE, 0, 42, MPI_COMM_WORLD);
        }
    }
}