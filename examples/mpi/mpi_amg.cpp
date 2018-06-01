#include <iostream>
#include <vector>
#include <string>

#include <boost/scope_exit.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <amgcl/backend/builtin.hpp>
#include <amgcl/value_type/static_matrix.hpp>
#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/solver/runtime.hpp>

#include <amgcl/mpi/util.hpp>
#include <amgcl/mpi/make_solver.hpp>
#include <amgcl/mpi/amg.hpp>
#include <amgcl/mpi/coarsening/runtime.hpp>
#include <amgcl/mpi/relaxation/runtime.hpp>
#include <amgcl/mpi/direct_solver/runtime.hpp>
#include <amgcl/mpi/repartition/runtime.hpp>

#include <amgcl/io/mm.hpp>
#include <amgcl/profiler.hpp>

#include "domain_partition.hpp"

namespace amgcl {
    profiler<> prof;
}

namespace math = amgcl::math;

//typedef amgcl::static_matrix<double, 2, 2>       val_type;
typedef double val_type;

typedef typename math::rhs_of<val_type>::type    rhs_type;
typedef typename math::scalar_of<val_type>::type scalar;

//---------------------------------------------------------------------------
struct renumbering {
    const domain_partition<3> &part;
    const std::vector<ptrdiff_t> &dom;

    renumbering(
            const domain_partition<3> &p,
            const std::vector<ptrdiff_t> &d
            ) : part(p), dom(d)
    {}

    ptrdiff_t operator()(ptrdiff_t i, ptrdiff_t j, ptrdiff_t k) const {
        boost::array<ptrdiff_t, 3> p = {{i, j, k}};
        std::pair<int,ptrdiff_t> v = part.index(p);
        return dom[v.first] + v.second;
    }
};

//---------------------------------------------------------------------------
template <class Backend, typename rhs_type>
boost::shared_ptr< amgcl::mpi::distributed_matrix<Backend> >
assemble_poisson3d(amgcl::mpi::communicator comm, ptrdiff_t n,
        std::vector<rhs_type>  &rhs)
{
    typedef typename Backend::value_type val_type;
    using amgcl::prof;

    boost::array<ptrdiff_t, 3> lo = { {0,   0,   0  } };
    boost::array<ptrdiff_t, 3> hi = { {n-1, n-1, n-1} };

    prof.tic("partition");
    domain_partition<3> part(lo, hi, comm.size);
    ptrdiff_t chunk = part.size(comm.rank);

    std::vector<ptrdiff_t> domain = comm.exclusive_sum(chunk);

    lo = part.domain(comm.rank).min_corner();
    hi = part.domain(comm.rank).max_corner();

    renumbering renum(part, domain);
    prof.toc("partition");

    std::vector<ptrdiff_t> ptr; ptr.reserve(chunk + 1);
    std::vector<ptrdiff_t> col; col.reserve(chunk * 7);
    std::vector<val_type>  val; val.reserve(chunk * 7);

    rhs.clear(); rhs.reserve(chunk);

    ptr.push_back(0);

    const val_type h2i = (n - 1) * (n - 1) * math::identity<val_type>();

    for(ptrdiff_t k = lo[2]; k <= hi[2]; ++k) {
        for(ptrdiff_t j = lo[1]; j <= hi[1]; ++j) {
            for(ptrdiff_t i = lo[0]; i <= hi[0]; ++i) {
                if (k > 0)  {
                    col.push_back(renum(i,j,k-1));
                    val.push_back(-h2i);
                }

                if (j > 0)  {
                    col.push_back(renum(i,j-1,k));
                    val.push_back(-h2i);
                }

                if (i > 0) {
                    col.push_back(renum(i-1,j,k));
                    val.push_back(-h2i);
                }

                col.push_back(renum(i,j,k));
                val.push_back(6 * h2i);

                if (i + 1 < n) {
                    col.push_back(renum(i+1,j,k));
                    val.push_back(-h2i);
                }

                if (j + 1 < n) {
                    col.push_back(renum(i,j+1,k));
                    val.push_back(-h2i);
                }

                if (k + 1 < n) {
                    col.push_back(renum(i,j,k+1));
                    val.push_back(-h2i);
                }

                ptr.push_back( col.size() );
                rhs.push_back( amgcl::math::constant<rhs_type>(1) );
            }
        }
    }

    return boost::make_shared< amgcl::mpi::distributed_matrix<Backend> >(
            comm, boost::tie(chunk, ptr, col, val));
}

//---------------------------------------------------------------------------
template <class Backend, typename rhs_type>
boost::shared_ptr< amgcl::mpi::distributed_matrix<Backend> >
read_matrix_market(
        amgcl::mpi::communicator comm,
        const std::string &A_file, const std::string &rhs_file,
        amgcl::runtime::mpi::repartition::type r,
        std::vector<rhs_type> &rhs)
{
    typedef amgcl::mpi::distributed_matrix<Backend> matrix;
    using amgcl::prof;

    std::vector<ptrdiff_t> ptr;
    std::vector<ptrdiff_t> col;
    std::vector<double>    val;
    std::vector<double>    f;

    prof.tic("read");
    amgcl::io::mm_reader A_mm(A_file);
    ptrdiff_t n = A_mm.rows();
    ptrdiff_t chunk = (n + comm.size - 1) / comm.size;
    ptrdiff_t row_beg = std::min(n, chunk * comm.rank);
    ptrdiff_t row_end = std::min(n, row_beg + chunk);
    chunk = row_end - row_beg;

    A_mm(ptr, col, val, row_beg, row_end);

    if (rhs_file.empty()) {
        f.resize(chunk);
        std::fill(f.begin(), f.end(), amgcl::math::constant<rhs_type>(1));
    } else {
        amgcl::io::mm_reader rhs_mm(rhs_file);
        rhs_mm(f, row_beg, row_end);
    }
    prof.toc("read");

    boost::shared_ptr<matrix> A = boost::make_shared<matrix>(comm, boost::tie(chunk, ptr, col, val));

    if (comm.size > 1 && r != amgcl::runtime::mpi::repartition::dummy) {
        prof.tic("partition");

        boost::property_tree::ptree prm;
        prm.put("type", r);
        prm.put("shrink_ratio", 1);
        amgcl::runtime::mpi::repartition::wrapper<Backend> partition(prm);

        boost::shared_ptr<matrix> I = partition(*A);
        boost::shared_ptr<matrix> J = transpose(*I);
        A = product(*J, *product(*A, *I));

        rhs.resize(J->loc_rows());

        J->move_to_backend();
        amgcl::backend::spmv(1, *J, f, 0, rhs);

        prof.toc("partition");
    } else {
        rhs.swap(f);
    }

    return A;
}

//---------------------------------------------------------------------------
template <class Backend, typename rhs_type>
void solve(amgcl::mpi::communicator comm,
        boost::shared_ptr< amgcl::mpi::distributed_matrix<Backend> > A,
        const boost::property_tree::ptree &prm,
        const std::vector<rhs_type> &rhs)
{
    typedef typename Backend::value_type val_type;
    using amgcl::prof;

    typedef
        amgcl::mpi::make_solver<
            amgcl::mpi::amg<
                Backend,
                amgcl::runtime::mpi::coarsening::wrapper<Backend>,
                amgcl::runtime::mpi::relaxation::wrapper<Backend>,
                amgcl::runtime::mpi::direct::solver<val_type>,
                amgcl::runtime::mpi::repartition::wrapper<Backend>
                >,
            amgcl::runtime::solver::wrapper
            >
        Solver;

    prof.tic("setup");
    Solver solve(comm, A, prm);
    prof.toc("setup");

    if (comm.rank == 0) {
        std::cout << solve << std::endl;
    }

    std::vector<rhs_type> x(A->loc_rows(), math::zero<rhs_type>());

    int    iters;
    double error;

    prof.tic("solve");
    boost::tie(iters, error) = solve(rhs, x);
    prof.toc("solve");

    if (comm.rank == 0) {
        std::cout
            << "Iterations: " << iters << std::endl
            << "Error:      " << error << std::endl
            << prof << std::endl;
    }
}

//---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    BOOST_SCOPE_EXIT(void) {
        MPI_Finalize();
    } BOOST_SCOPE_EXIT_END

    amgcl::mpi::communicator comm(MPI_COMM_WORLD);

    if (comm.rank == 0)
        std::cout << "World size: " << comm.size << std::endl;

    using amgcl::prof;

    // Read configuration from command line
    namespace po = boost::program_options;
    po::options_description desc("Options");

    desc.add_options()
        ("help,h", "show help")
        ("matrix,A",
         po::value<std::string>(),
         "System matrix in the MatrixMarket format. "
         "When not specified, a Poisson problem in 3D unit cube is assembled. "
        )
        (
         "rhs,f",
         po::value<std::string>()->default_value(""),
         "The RHS vector in the MatrixMarket format. "
         "When omitted, a vector of ones is used by default. "
         "Should only be provided together with a system matrix. "
        )
        (
         "partitioner,r",
         po::value<amgcl::runtime::mpi::repartition::type>()->default_value(
#if defined(AMGCL_HAVE_SCOTCH)
             amgcl::runtime::mpi::repartition::scotch
#elif defined(AMGCL_HAVE_PASTIX)
             amgcl::runtime::mpi::repartition::parmetis
#else
             amgcl::runtime::mpi::repartition::dummy
#endif
             ),
         "Repartition the system matrix"
        )
        (
         "size,n",
         po::value<ptrdiff_t>()->default_value(128),
         "domain size"
        )
        ("prm-file,P",
         po::value<std::string>(),
         "Parameter file in json format. "
        )
        (
         "prm,p",
         po::value< std::vector<std::string> >()->multitoken(),
         "Parameters specified as name=value pairs. "
         "May be provided multiple times. Examples:\n"
         "  -p solver.tol=1e-3\n"
         "  -p precond.coarse_enough=300"
        )
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        if (comm.rank == 0) std::cout << desc << std::endl;
        return 0;
    }

    boost::property_tree::ptree prm;
    if (vm.count("prm-file")) {
        read_json(vm["prm-file"].as<std::string>(), prm);
    }

    if (vm.count("prm")) {
        BOOST_FOREACH(std::string v, vm["prm"].as<std::vector<std::string> >()) {
            amgcl::put(prm, v);
        }
    }

    typedef amgcl::backend::builtin<val_type> Backend;

    ptrdiff_t n = vm["size"].as<ptrdiff_t>();
    boost::shared_ptr< amgcl::mpi::distributed_matrix<Backend> > A;
    std::vector<rhs_type>  rhs;

    prof.tic("assemble");
    if (vm.count("matrix")) {
        A = read_matrix_market<Backend>(comm,
                vm["matrix"].as<std::string>(),
                vm["rhs"].as<std::string>(),
                vm["partitioner"].as<amgcl::runtime::mpi::repartition::type>(),
                rhs);
    } else {
        A = assemble_poisson3d<Backend>(comm, n, rhs);
    }
    prof.toc("assemble");

    solve(comm, A, prm, rhs);
}
