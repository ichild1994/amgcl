#ifndef AMGCL_SPMAT_HPP
#define AMGCL_SPMAT_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cassert>
#include <omp.h>

// Local implementation of a sparse matrix. Use amg::sparse::map with
// externally-defined matrices in CRS format.

namespace amg {
namespace sparse {

//---------------------------------------------------------------------------
template <class spmat>
struct matrix_index {
    typedef typename spmat::index_type type;
};

//---------------------------------------------------------------------------
template <class spmat>
struct matrix_value {
    typedef typename spmat::value_type type;
};

//---------------------------------------------------------------------------
template <class spmat>
typename matrix_index<spmat>::type matrix_rows(const spmat &A) {
    return A.rows;
}

template <class spmat>
typename matrix_index<spmat>::type matrix_cols(const spmat &A) {
    return A.cols;
}

//---------------------------------------------------------------------------
template <class spmat>
const typename matrix_index<spmat>::type * matrix_outer_index(const spmat &A) {
    return &(A.row[0]);
}

template <class spmat>
typename matrix_index<spmat>::type * matrix_outer_index(spmat &A) {
    return &(A.row[0]);
}

//---------------------------------------------------------------------------
template <class spmat>
const typename matrix_index<spmat>::type * matrix_inner_index(const spmat &A) {
    return &(A.col[0]);
}

template <class spmat>
typename matrix_index<spmat>::type * matrix_inner_index(spmat &A) {
    return &(A.col[0]);
}

//---------------------------------------------------------------------------
template <class spmat>
const typename matrix_value<spmat>::type * matrix_values(const spmat &A) {
    return &(A.val[0]);
}

template <class spmat>
typename matrix_value<spmat>::type * matrix_values(spmat &A) {
    return &(A.val[0]);
}

//---------------------------------------------------------------------------
template <class spmat>
typename matrix_index<spmat>::type matrix_nonzeros(const spmat &A) {
    return matrix_outer_index(A)[matrix_rows(A)];
}

//---------------------------------------------------------------------------
template <typename value_t = double, class index_t = long long>
struct matrix {
    typedef index_t index_type;
    typedef value_t value_type;

    matrix() : rows(0), cols(0) {}

    matrix(index_t rows, index_t cols, index_t nnz = 0) :
        rows(rows), cols(cols), row(rows + 1), col(nnz), val(nnz)
    {}

    matrix(const matrix &A) :
        rows(A.rows),
        cols(A.cols),
        row( A.row ),
        col( A.col ),
        val( A.val )
    {}

    matrix(matrix &&A) :
        rows(A.rows),
        cols(A.cols),
        row( std::move(A.row) ),
        col( std::move(A.col) ),
        val( std::move(A.val) )
    {}

    template <class spmat>
    matrix(const spmat &A) :
        rows(matrix_rows(A)),
        cols(matrix_cols(A)),
        row(matrix_outer_index(A), matrix_outer_index(A) + rows + 1),
        col(matrix_inner_index(A), matrix_inner_index(A) + row[rows]),
        val(matrix_values(A), matrix_values(A) + row[rows])
    {}

    void reserve(index_t nnz) {
        col.resize(nnz);
        val.resize(nnz);
    }

    void clear() {
        rows = cols = 0;
        std::vector<index_t>().swap(row);
        std::vector<index_t>().swap(col);
        std::vector<value_t>().swap(val);
    }

    index_t rows, cols;

    std::vector<index_t> row;
    std::vector<index_t> col;
    std::vector<value_t> val;
};

//---------------------------------------------------------------------------
template <typename value_t, class index_t>
struct matrix_map {
    typedef index_t index_type;
    typedef value_t value_type;

    matrix_map(
            index_t rows, index_t cols,
            index_t *row, index_t *col, value_t *val
            )
        : rows(rows), cols(cols), row(row), col(col), val(val)
    {}

    index_t rows, cols;

    index_t *row;
    index_t *col;
    value_t *val;
};

template <typename value_t, class index_t>
matrix_map<value_t, index_t> map(
        index_t rows, index_t cols, index_t *row, index_t *col, value_t *val
        )
{
    return matrix_map<value_t, index_t>(rows, cols, row, col, val);
}

//---------------------------------------------------------------------------
template <class spmat>
matrix<
    typename matrix_value<spmat>::type,
    typename matrix_index<spmat>::type
    >
transpose(const spmat &A) {
    typedef typename matrix_index<spmat>::type index_t;
    typedef typename matrix_value<spmat>::type value_t;

    const index_t n   = matrix_rows(A);
    const index_t m   = matrix_cols(A);
    const index_t nnz = matrix_nonzeros(A);

    auto Arow = matrix_outer_index(A);
    auto Acol = matrix_inner_index(A);
    auto Aval = matrix_values(A);

    matrix<value_t, index_t> T(m, n, nnz);

    std::fill(T.row.begin(), T.row.end(), static_cast<index_t>(0));

    for(index_t j = 0; j < nnz; ++j)
        ++( T.row[Acol[j] + 1] );

    std::partial_sum(T.row.begin(), T.row.end(), T.row.begin());

    for(index_t i = 0; i < n; i++) {
        for(index_t j = Arow[i], e = Arow[i + 1]; j < e; ++j) {
            index_t head = T.row[Acol[j]]++;

            T.col[head] = i;
            T.val[head] = Aval[j];
        }
    }

    std::copy(T.row.rbegin() + 1, T.row.rend(), T.row.rbegin());
    T.row[0] = 0;

    return T;
}

//---------------------------------------------------------------------------
template <class spmat1, class spmat2>
matrix<
    typename matrix_value<spmat1>::type,
    typename matrix_index<spmat1>::type
    >
prod(const spmat1 &A, const spmat2 &B) {
    typedef typename matrix_index<spmat1>::type index_t;
    typedef typename matrix_value<spmat1>::type value_t;

    const index_t n   = matrix_rows(A);
    const index_t m   = matrix_cols(B);

    auto Arow = matrix_outer_index(A);
    auto Acol = matrix_inner_index(A);
    auto Aval = matrix_values(A);

    auto Brow = matrix_outer_index(B);
    auto Bcol = matrix_inner_index(B);
    auto Bval = matrix_values(B);

    matrix<value_t, index_t> C(n, m);

    std::fill(C.row.begin(), C.row.end(), static_cast<index_t>(0));

#pragma omp parallel
    {
        std::vector<index_t> marker(m, static_cast<index_t>(-1));

	int nt  = omp_get_num_threads();
	int tid = omp_get_thread_num();

	index_t chunk_size  = (n + nt - 1) / nt;
	index_t chunk_start = tid * chunk_size;
	index_t chunk_end   = std::min(n, chunk_start + chunk_size);

        for(index_t ia = chunk_start; ia < chunk_end; ++ia) {
            for(index_t ja = Arow[ia], ea = Arow[ia + 1]; ja < ea; ++ja) {
                index_t ca = Acol[ja];
                for(index_t jb = Brow[ca], eb = Brow[ca + 1]; jb < eb; ++jb) {
                    index_t cb = Bcol[jb];

                    if (marker[cb] != ia) {
                        marker[cb] = ia;
                        ++( C.row[ia + 1] );
                    }
                }
            }
        }

        std::fill(marker.begin(), marker.end(), static_cast<index_t>(-1));

#pragma omp barrier
#pragma omp single
        {
            std::partial_sum(C.row.begin(), C.row.end(), C.row.begin());
            C.reserve(C.row.back());
        }

        for(index_t ia = chunk_start; ia < chunk_end; ++ia) {
            index_t row_beg = C.row[ia];
            index_t row_end = row_beg;

            for(index_t ja = Arow[ia], ea = Arow[ia + 1]; ja < ea; ++ja) {
                index_t ca = Acol[ja];
                value_t va = Aval[ja];

                for(index_t jb = Brow[ca], eb = Brow[ca + 1]; jb < eb; ++jb) {
                    index_t cb = Bcol[jb];
                    value_t vb = Bval[jb];

                    if (marker[cb] < row_beg) {
                        marker[cb] = row_end;
                        C.col[row_end] = cb;
                        C.val[row_end] = va * vb;
                        ++row_end;
                    } else {
                        C.val[marker[cb]] += va * vb;
                    }
                }
            }
        }
    }

    return C;
}

//---------------------------------------------------------------------------
template <typename index_t, class value_t>
void gaussj(index_t n, value_t *a) {
    const static value_t one = static_cast<value_t>(1);
    const static value_t zero = static_cast<value_t>(0);

    std::vector<index_t> idxc(n);
    std::vector<index_t> idxr(n);
    std::vector<char>    ipiv(n, false);

    for(index_t i = 0; i < n; ++i) {
        index_t irow, icol;

        value_t big = zero;
        for(index_t j = 0; j < n; ++j) {
            if (ipiv[j]) continue;

            for(index_t k = 0; k < n; ++k) {
                if (!ipiv[k] && std::abs(a[j * n + k]) > big) {
                    big  = std::abs(a[j * n + k]);
                    irow = j;
                    icol = k;
                }
            }
        }

        ipiv[icol] = true;

        if (irow != icol)
            std::swap_ranges(
                    a + n * irow, a + n * (irow + 1),
                    a + n * icol
                    );

        idxr[i] = irow;
        idxc[i] = icol;

        if (a[icol * n + icol] == zero)
            throw std::logic_error("Singular matrix in gaussj");

        value_t pivinv = one / a[icol * n + icol];
        a[icol * n + icol] = one;

        std::transform(a + icol * n, a + (icol + 1) * n, a + icol * n,
                [pivinv](value_t e) {
                    return e * pivinv;
                });

        for(index_t k = 0; k < n; ++k) {
            if (k != icol) {
                value_t dum = a[k * n + icol];
                a[k * n + icol] = zero;
                std::transform(
                        a + n * k, a + n * (k + 1),
                        a + n * icol,
                        a + n * k,
                        [dum](value_t v1, value_t v2) {
                            return v1 - v2 * dum;
                        });
            }
        }
    }

    for(index_t i = n - 1; i >= 0; --i) {
        if (idxr[i] != idxc[i]) {
            for(index_t j = 0; j < n; ++j)
                std::swap(a[j * n + idxr[i]], a[j * n + idxc[i]]);
        }
    }
}

//---------------------------------------------------------------------------
template <class spmat>
matrix<
    typename matrix_value<spmat>::type,
    typename matrix_index<spmat>::type
    >
inverse(const spmat &A) {
    typedef typename matrix_index<spmat>::type index_t;
    typedef typename matrix_value<spmat>::type value_t;

    const index_t n = sparse::matrix_rows(A);

    assert(n == sparse::matrix_cols(A)
            && "Inverse of a non-square matrix does not make sense");

    auto Arow = matrix_outer_index(A);
    auto Acol = matrix_inner_index(A);
    auto Aval = matrix_values(A);

    matrix<
        typename matrix_value<spmat>::type,
        typename matrix_index<spmat>::type
    > Ainv(n, n, n * n);

    std::fill(Ainv.val.begin(), Ainv.val.end(), static_cast<value_t>(0));

    for(index_t i = 0; i < n; ++i)
        for(index_t j = Arow[i], e = Arow[i + 1]; j < e; ++j)
            Ainv.val[i * n + Acol[j]] = Aval[j];

    gaussj(n, Ainv.val.data());

    Ainv.row[0] = 0;
    for(index_t i = 0, idx = 0; i < n; ) {
        for(index_t j = 0; j < n; ++j, ++idx) Ainv.col[idx] = j;

        Ainv.row[++i] = idx;
    }

    return Ainv;
}

//---------------------------------------------------------------------------
template <class spmat>
std::vector< typename matrix_value<spmat>::type >
diagonal(const spmat &A) {
    typedef typename matrix_index<spmat>::type index_t;
    typedef typename matrix_value<spmat>::type value_t;

    const index_t n = sparse::matrix_rows(A);

    assert(n == sparse::matrix_cols(A)
            && "Diagonal of a non-square matrix is not well-defined");

    auto Arow = matrix_outer_index(A);
    auto Acol = matrix_inner_index(A);
    auto Aval = matrix_values(A);

    std::vector<value_t> dia(n);

    for(index_t i = 0; i < n; ++i) {
        value_t d = 0;
        for(index_t j = Arow[i], e = Arow[i + 1]; j < e; ++j) {
            if (Acol[j] == i) {
                d = Aval[j];
                break;
            }
        }
        dia[i] = d;
    }

    return dia;
}

} // namespace sparse
} // namespace amg

#endif
