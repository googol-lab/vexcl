#ifndef VEXCL_FFT_HPP
#define VEXCL_FFT_HPP

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

/**
 * \file   stencil.hpp
 * \author Pascal Germroth <pascal@ensieve.org>
 * \brief  Fast Fourier Transformation.
 */

// TODO: multivector (AMD FFT supports batch transforms!)
#include <vexcl/vector.hpp>

// AMD's FFT library.
#include <clAmdFft.h>

namespace vex {

/// \cond INTERNAL

template <class F>
struct fft_expr
    : vector_expression< boost::proto::terminal< additive_vector_transform >::type >
{
    const F &f;
    const vector<typename F::input_t> &input;

    fft_expr(const F &f, const vector<typename F::input_t> &x) : f(f), input(x) {}

    template <bool negate, bool append>
    void apply(vector<typename F::output_t> &output) const
    {
        f.template execute<negate, append>(input, output);
    }
};


enum fft_direction {
   forward = CLFFT_FORWARD,
   inverse = CLFFT_BACKWARD
};

// AMD FFT needs Setup/Teardown calls for the whole library.
// Sequential Setup/Teardowns are OK, but overlapping is not.
size_t fft_ref_count = 0;
// TODO: breaks when two objects, both using FFT, are linked.


/**
 * An FFT functor. Assumes the vector is in row major format and densely packed.
 * Only supports a single device, only 2^a 3^b 5^c sizes, only single precision.
 * 1-3 dimensions.
 * Usage:
 * \code
 * FFT<cl_float2> fft(ctx.queue(), {width, height});
 * output = fft(input); // out-of-place transform
 * data = fft(data); // in-place transform
 * \endcode
 */
template <typename T0, typename T1 = T0>
struct FFT {
    static_assert(std::is_same<T0, T1>::value &&
        std::is_same<T0, cl_float2>::value,
        "Only single precision Complex-to-Complex transformations implemented.");

    typedef FFT<T0, T1> this_t;
    typedef T0 input_t;
    typedef T1 output_t;

    const std::vector<cl::CommandQueue> &queues;
    clAmdFftPlanHandle plan;
    fft_direction dir;

    void check_error(clAmdFftStatus status) const {
        if(status != CL_SUCCESS)
            throw cl::Error(status, "AMD FFT");
    }

    template <class Array>
    FFT(const std::vector<cl::CommandQueue> &queues,
        const Array &lengths, fft_direction dir = forward)
        : queues(queues), plan(0), dir(dir) {
        init(lengths);
    }

    FFT(const std::vector<cl::CommandQueue> &queues,
        size_t length, fft_direction dir = forward)
        : queues(queues), plan(0), dir(dir) {
        std::array<size_t, 1> lengths = {{length}};
        init(lengths);
    }
   
#ifndef BOOST_NO_INITIALIZER_LISTS
    FFT(const std::vector<cl::CommandQueue> &queues,
        std::initializer_list<size_t> lengths, fft_direction dir = forward)
        : queues(queues), plan(0), dir(dir) {
        init(lengths);
    }
#endif

    template <class Array>
    void init(const Array &lengths) {
        assert(lengths.size() >= 1 && lengths.size() <= 3);
        size_t _lengths[lengths.size()];
        std::copy(std::begin(lengths), std::end(lengths), _lengths);
        // TODO: all queues must be in same context
        cl::Context context = static_cast<cl::CommandQueue>(queues[0]).getInfo<CL_QUEUE_CONTEXT>();
        if(fft_ref_count++ == 0)
            check_error(clAmdFftSetup(NULL));
        check_error(clAmdFftCreateDefaultPlan(&plan, context(),
            static_cast<clAmdFftDim>(lengths.size()), _lengths));
        check_error(clAmdFftSetPlanPrecision(plan, CLFFT_SINGLE)); 
        check_error(clAmdFftSetLayout(plan, CLFFT_COMPLEX_INTERLEAVED, CLFFT_COMPLEX_INTERLEAVED));
    }

    ~FFT() {
        if(plan)
            check_error(clAmdFftDestroyPlan(&plan));
        if(--fft_ref_count == 0)
            check_error(clAmdFftTeardown());
    }
    

    template <bool negate, bool append>
    void execute(const vector<T0> &input, vector<T1> &output) const {
        assert(!append); // TODO: that should be static.
        static_assert(!negate, "Negation not implemented yet.");
        // Doesn't support split buffers.
        assert(queues.size() == 1);
        cl_mem input_buf = input(0)();
        cl_mem output_buf = output(0)();
        check_error(clAmdFftSetResultLocation(plan,
            input_buf == output_buf ? CLFFT_INPLACE : CLFFT_OUTOFPLACE));
        cl_command_queue _queues[queues.size()];
        for(size_t i = 0 ; i < queues.size() ; i++)
            _queues[i] = queues[i]();
        check_error(clAmdFftEnqueueTransform(plan, static_cast<clAmdFftDirection>(dir),
            queues.size(), _queues,
            /* wait events */0, NULL, /* out events */NULL,
            &input_buf, &output_buf, NULL));
    }


    // User call
    fft_expr<this_t> operator()(const vector<T0> &x) const {
        return {*this, x};
    }
};


}

#endif
