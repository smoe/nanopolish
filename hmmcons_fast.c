#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <algorithm>
#include <sstream>
#include <set>
#include <omp.h>
#include "hmmcons_poremodel.h"
#include "hmmcons_interface.h"
#include "hmmcons_khmm_parameters.h"
#include "Profiler.h"

// Macros
#define max3(x,y,z) std::max(std::max(x,y), z)

// Constants

// strands
const uint8_t T_IDX = 0;
const uint8_t C_IDX = 1;
const uint8_t NUM_STRANDS = 2;

// 
const uint8_t K = 5;

const static double LOG_KMER_INSERTION = log(0.1);
const static double P_RANDOM_SKIP = 0.05;
const static double EVENT_DETECTION_THRESHOLD = 1.0f;
const static uint32_t KHMM_MAX_JUMP = 5;
const static uint32_t KHMM_MAX_MERGE = 10;

static const uint8_t base_rank[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,1,0,0,0,2,0,0,0,0,0,0,0,0,
    0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

enum AlignmentPolicy
{
    AP_GLOBAL,
    AP_SEMI_KMER
};

//#define DEBUG_HMM_UPDATE 1
//#define DEBUG_HMM_EMISSION 1
//#define DEBUG_TRANSITION 1
#define PRINT_TRAINING_MESSAGES 1
//#define DEBUG_SINGLE_SEGMENT 1
//#define DEBUG_SHOW_TOP_TWO 1
#define DEBUG_PATH_SELECTION 1

struct CEventSequence
{
    uint32_t n_events;
    const double* level;
    const double* stdv;
    const double* time;
};

struct CSquiggleRead
{
    // unique identifier of the read
    uint32_t read_id;

    // one model for each strand
    CPoreModel pore_model[2];

    // one event sequence for each strand
    CEventSequence events[2];

    // one set of parameters per strand
    KHMMParameters parameters[2];
};

double get_duration(const CSquiggleRead& read, uint32_t event_idx, uint32_t strand)
{
    double e_start = read.events[strand].time[event_idx];
    double e_end = read.events[strand].time[event_idx + 1];
    return e_end - e_start;
}

double get_drift_corrected_level(const CSquiggleRead& read, uint32_t event_idx, uint32_t strand)
{
    double level = read.events[strand].level[event_idx];
    // correct level by drift
    double start = read.events[strand].time[0];
    double time = read.events[strand].time[event_idx] - start;
    return level - (time * read.pore_model[strand].drift);
}

struct HMMReadAnchor
{
    int32_t event_idx;
    bool rc; // with respect to consensus
};

struct HMMAnchoredColumn
{
    std::vector<HMMReadAnchor> anchors;
    std::string base_sequence;
    std::vector<std::string> alt_sequences;
};

struct HMMConsReadState
{
    CSquiggleRead* read;
    uint32_t anchor_index;
    uint32_t event_start_idx;
    uint32_t event_stop_idx;
    uint8_t strand;
    int8_t stride;
    uint8_t rc;
    std::string alignment;
};

struct PosteriorState
{
    uint32_t event_idx;
    uint32_t kmer_idx;
    double l_posterior;
    double l_fm;
    double log_transition_probability;
    char state;
};

// A global vector used to store data we've received from the python code
struct HmmConsData
{
    int num_threads;

    //
    std::vector<CSquiggleRead> reads;
    std::vector<HMMAnchoredColumn> anchored_columns;
    
    //
    std::string consensus_result;
};

HmmConsData g_data;
bool g_initialized = false;

// 
// Add the log-scaled values a and b using a transform to avoid
// precision errors
inline double add_logs(const double a, const double b)
{
    if(a == -INFINITY && b == -INFINITY)
        return -INFINITY;

    if(a > b) {
        double diff = b - a;
        return a + log(1.0 + exp(diff));
    } else {
        double diff = a - b;
        return b + log(1.0 + exp(diff));
    }
}

extern "C"
void initialize(int num_threads)
{
    g_initialized = true;
    g_data.num_threads = num_threads;
}

extern "C"
void clear_data()
{
    g_data.reads.clear();
    g_data.anchored_columns.clear();
    g_data.consensus_result.clear();
}

extern "C"
const char* get_consensus_result()
{
    return g_data.consensus_result.c_str();
}

extern "C"
void add_read(CSquiggleReadInterface params)
{
    g_data.reads.push_back(CSquiggleRead());

    CSquiggleRead& sr = g_data.reads.back();
    sr.read_id = g_data.reads.size() - 1;

    for(uint32_t i = 0; i < NUM_STRANDS; ++i) {
        // Initialize pore model   
        sr.pore_model[i].scale = params.pore_model[i].scale;
        sr.pore_model[i].shift = params.pore_model[i].shift;
        sr.pore_model[i].drift = params.pore_model[i].drift;
        sr.pore_model[i].var = params.pore_model[i].var;
        
        assert(params.pore_model[i].n_states == 1024);
        for(uint32_t j = 0; j < params.pore_model[i].n_states; ++j) {
            
            sr.pore_model[i].state[j].level_mean = params.pore_model[i].level_mean[j];
            sr.pore_model[i].state[j].level_stdv = params.pore_model[i].level_stdv[j];
            
            sr.pore_model[i].state[j].sd_mean = params.pore_model[i].sd_mean[j];
            sr.pore_model[i].state[j].sd_stdv = params.pore_model[i].sd_stdv[j];
         }
    
        // Initialize events
        sr.events[i].n_events = params.events[i].n_events;
        sr.events[i].level = params.events[i].level;
        sr.events[i].stdv = params.events[i].stdv;
        sr.events[i].time = params.events[i].time;

        /*
        printf("Model[%zu] scale: %lf shift: %lf %lf %lf\n", i, sr.pore_model[i].scale, 
                                                                 sr.pore_model[i].shift,
                                                                 sr.pore_model[i].state[0].level_mean, 
                                                                 sr.pore_model[i].state[0].level_stdv);
    
        printf("First 100 events of %d\n", sr.events[i].n_events);
        for(int j = 0; j < 100; ++j)
            printf("%d: %lf\n", j, sr.events[i].level[j]);
        */
    }

    // Initialize hmm parameters for both strands of the read
    khmm_parameters_initialize(sr.parameters[0]);
    khmm_parameters_initialize(sr.parameters[1]);
}

// This is called by python to tell us we want to start a new anchored column
extern "C"
void start_anchored_column()
{
    HMMAnchoredColumn ac;
    g_data.anchored_columns.push_back(ac);
}

extern "C"
void add_read_anchor(CReadAnchorInterface in_ra)
{
    assert(!g_data.anchored_columns.empty());

    HMMReadAnchor ra = { in_ra.event_idx, in_ra.rc };
    g_data.anchored_columns.back().anchors.push_back(ra);
}

extern "C"
void add_base_sequence(char* str)
{
    assert(!g_data.anchored_columns.empty());
    g_data.anchored_columns.back().base_sequence = str;
}

extern "C"
void add_alt_sequence(char* str)
{
    assert(!g_data.anchored_columns.empty());
    g_data.anchored_columns.back().alt_sequences.push_back(str);
}

// This is called by python to tell us we want to start a new anchored column
extern "C"
void end_anchored_column()
{
    // Validate that we received two read anchors per read
    assert(g_data.anchored_columns.back().anchors.size() == g_data.reads.size() * 2);
}

// Make a unique index for the strand this read state represents
uint32_t get_strand_idx(const HMMConsReadState& rs)
{
    return rs.read->read_id + rs.strand;
}

//
// HMM matrix
//
struct HMMCell
{
    double M;
    double E;
    double K;
};

struct HMMMatrix
{
    HMMCell* cells;
    uint32_t n_rows;
    uint32_t n_cols;
};

std::vector<HMMConsReadState> get_read_states_for_columns(const HMMAnchoredColumn& start_column,  
                                                          const HMMAnchoredColumn& end_column)
{
    assert(start_column.anchors.size() == end_column.anchors.size());

    std::vector<HMMConsReadState> read_states;
    for(uint32_t rsi = 0; rsi < start_column.anchors.size(); ++rsi) {

        HMMReadAnchor start_ra = start_column.anchors[rsi];
        HMMReadAnchor end_ra = end_column.anchors[rsi];

        // This read strand does not have events at both anchors
        if(start_ra.event_idx == -1 || end_ra.event_idx == -1)
            continue;

        HMMConsReadState crs;

        uint32_t read_idx = rsi / 2;
        assert(read_idx < g_data.reads.size());
        crs.anchor_index = rsi;
        crs.read = &g_data.reads[read_idx];
        crs.strand = rsi % 2;
        crs.event_start_idx = start_ra.event_idx;
        crs.event_stop_idx = end_ra.event_idx;
        if(crs.event_start_idx < crs.event_stop_idx)
            crs.stride = 1;
        else
            crs.stride = -1;
        assert(start_ra.rc == end_ra.rc);
        crs.rc = start_ra.rc;

        read_states.push_back(crs);
    }
    return read_states;
}

void allocate_matrix(HMMMatrix& matrix, uint32_t n_rows, uint32_t n_cols)
{
    matrix.n_rows = n_rows;
    matrix.n_cols = n_cols;
    uint32_t N = matrix.n_rows * matrix.n_cols;
    matrix.cells = (HMMCell*)malloc(N * sizeof(HMMCell));
    memset(matrix.cells, 0, N * sizeof(HMMCell));
}

void free_matrix(HMMMatrix& matrix)
{
    free(matrix.cells);
    matrix.cells = NULL;
}

inline uint32_t cell(const HMMMatrix& matrix, uint32_t row, uint32_t col)
{
    return row * matrix.n_cols + col;
}

void print_matrix(const HMMMatrix& matrix)
{
    for(uint32_t i = 0; i < matrix.n_rows; ++i) {
        for(uint32_t j = 0; j < matrix.n_cols; ++j) {
            uint32_t c = cell(matrix, i, j);
            printf("%.1lf,%.1lf,%.1f\t", matrix.cells[c].M, matrix.cells[c].E, matrix.cells[c].K);
        }
        printf("\n");
    }
}

//
// Template Matrix for POD types
//
template<typename T>
struct Matrix
{
    T* cells;
    uint32_t n_rows;
    uint32_t n_cols;
};

typedef Matrix<double> DoubleMatrix;
typedef Matrix<uint32_t> UInt32Matrix;

template<typename T>
void allocate_matrix(Matrix<T>& matrix, uint32_t n_rows, uint32_t n_cols)
{
    matrix.n_rows = n_rows;
    matrix.n_cols = n_cols;
    
    uint32_t N = matrix.n_rows * matrix.n_cols;
    matrix.cells = (T*)malloc(N * sizeof(T));
    memset(matrix.cells, 0, N * sizeof(T));
}

template<typename T>
void free_matrix(Matrix<T>& matrix)
{
    assert(matrix.cells != NULL);
    free(matrix.cells);
    matrix.cells = NULL;
}

template<typename T>
inline uint32_t cell(const Matrix<T>& matrix, uint32_t row, uint32_t col)
{
    return row * matrix.n_cols + col;
}

template<typename T, typename U>
inline void set(Matrix<T>& matrix, uint32_t row, uint32_t col, U v)
{
    uint32_t c = cell(matrix, row, col);
    matrix.cells[c] = v;
}

template<typename T>
inline T get(const Matrix<T>& matrix, uint32_t row, uint32_t col)
{
    uint32_t c = cell(matrix, row, col);
    return matrix.cells[c];
}

//
//
//
void print_matrix(const DoubleMatrix& matrix, bool do_exp = false)
{
    for(uint32_t i = 0; i < matrix.n_rows; ++i) {
        for(uint32_t j = 0; j < matrix.n_cols; ++j) {
            uint32_t c = cell(matrix, i, j);
            double v = matrix.cells[c];
            if(do_exp)
                v = exp(v);
            printf("%.3lf\t", v);
        }
        printf("\n");
    }
}

//
// Kmer Ranks
//
inline uint32_t kmer_rank(const char* str, uint32_t K)
{
    uint32_t rank = 0;
    for(uint32_t i = 0; i < K; ++i)
        rank |= base_rank[str[i]] << 2 * (K - i - 1);
    return rank;
}

inline uint32_t rc_kmer_rank(const char* str, uint32_t K)
{
    uint32_t rank = 0;
    for(int32_t i = K - 1; i >= 0; --i)
        rank |= ((3 - base_rank[str[i]]) << 2 * i);
    return rank;
}

// wrapper to get the rank for a kmer on the right strand
inline uint32_t get_rank(const HMMConsReadState& state, const char* s, uint32_t ki)
{
    const char* p = s + ki;
    return !state.rc ?  kmer_rank(p, K) : rc_kmer_rank(p, K);
}

// Increment the input string to be the next sequence in lexicographic order
void lexicographic_next(std::string& str)
{
    int carry = 1;
    int i = str.size() - 1;
    do {
        uint32_t r = base_rank[str[i]] + carry;
        str[i] = "ACGT"[r % 4];
        carry = r / 4;
        i -= 1;
    } while(carry > 0 && i >= 0);
}

// From SO: http://stackoverflow.com/questions/10847007/using-the-gaussian-probability-density-function-in-c
// TODO: replace with a lookup table that can be interpolated
inline double normal_pdf(double x, double m, double s)
{
    static const float inv_sqrt_2pi = 0.3989422804014327;
    double a = (x - m) / s;
    return inv_sqrt_2pi / s * exp(-0.5f * a * a);
}

inline double log_normal_pdf(double x, double m, double s)
{
    static const double log_inv_sqrt_2pi = log(0.3989422804014327);
    double a = (x - m) / s;
    return log_inv_sqrt_2pi - log(s) + (-0.5f * a * a);
}

// The probability that a standard normal RV is <= x
// from http://stackoverflow.com/questions/2328258/cumulative-normal-distribution-function-in-c-c
inline double log_standard_normal_cdf(double x)
{
    return 0.5 * erfc(-x * M_SQRT1_2);
}

// The probability that a normal RV is <= x
inline double log_normal_cdf(double x, double m, double s)
{
    double a = (x - m) / s;
    return log(0.5 * (1 + erf(a * M_SQRT1_2)));
}

inline double log_probability_range_match(const CSquiggleRead& read,
                                          uint32_t kmer_rank,
                                          uint32_t event_start, 
                                          uint32_t event_end,
                                          uint32_t event_stride,
                                          uint8_t strand)
{
    const CPoreModel& pm = read.pore_model[strand];

    // Extract event

    // Average the levels for the range of events
    // Sum durations as well
    /*
    double level = 0.0f;
    double duration = 0.0f;
    double n_events = 0.0f;
    for(uint32_t ei = event_start; ei != event_end; ei += event_stride) {
        double d = get_duration(read, ei, strand);
        level += (d * get_drift_corrected_level(read, ei, strand));
        duration += d;
        n_events += 1.0;
    }

    level = level / duration;
    level = level / n_events;

    double m = pm.state[kmer_rank].level_mean * pm.scale + pm.shift;
    double s = pm.state[kmer_rank].level_stdv * pm.var;
    double lp = log_normal_pdf(level, m, s);

    double rate = 27.777f;
    double ld = log(rate) - rate * fabs(duration);
#if DEBUG_HMM_EMISSION
    printf("\trange %zu %zu l: %.2lf n: %.2lf m: %.2lf s: %.2lf\n", event_start, event_end, level, n_events, m, s);
#endif

    return lp + ld;
    */

    // swap to increasing order
    if(event_stride == -1) {
        uint32_t tmp = event_start;
        event_start = event_end;
        event_end = tmp;
    }

    double m = pm.state[kmer_rank].level_mean * pm.scale + pm.shift;
    double s = pm.state[kmer_rank].level_stdv * pm.var;
    double duration = 0.0f;
    double lp = 0.0f;

    for(uint32_t ei = event_start; ei <= event_end; ei += 1) {
        double d = get_duration(read, ei, strand);
        double level = get_drift_corrected_level(read, ei, strand);
        duration += d;
        lp += (d * log_normal_pdf(level, m, s));
    }
    lp /= duration;
    
    double rate = 27.777f;
    double ld = log(rate) - rate * fabs(duration);
    
    return lp + ld;
}


inline double log_probability_match(const CSquiggleRead& read,
                                    uint32_t kmer_rank,
                                    uint32_t event_idx, 
                                    uint8_t strand)
{
    const CPoreModel& pm = read.pore_model[strand];

    // Extract event
    double level = get_drift_corrected_level(read, event_idx, strand);

    double m = pm.state[kmer_rank].level_mean * pm.scale + pm.shift;
    double s = pm.state[kmer_rank].level_stdv * pm.var;
    double lp = log_normal_pdf(level, m, s);

#if DEBUG_HMM_EMISSION
    printf("Event[%d] Kmer: %d -- L:%.1lf m: %.1lf s: %.1lf p: %.3lf p_old: %.3lf\n", event_idx, kmer_rank, level, m, s, exp(lp), normal_pdf(level, m, s));
#endif

    return lp;
}

inline double log_probability_event_insert(const CSquiggleRead& read,
                                           uint32_t kmer_rank,
                                           uint32_t event_idx, 
                                           uint8_t strand)
{
    return log_probability_match(read, kmer_rank, event_idx, strand);
}

inline double log_probability_kmer_insert(const CSquiggleRead& read,
                                          uint32_t kmer_rank,
                                          uint32_t event_idx, 
                                          uint8_t strand)

{
    return log_probability_match(read, kmer_rank, event_idx, strand);
}

void fill_khmm_transitions(DoubleMatrix& matrix, const std::string& consensus, const HMMConsReadState& state)
{
    PROFILE_FUNC("fill_khmm_transitions")

    const CPoreModel& pm = state.read->pore_model[state.strand];
    const KHMMParameters& parameters = state.read->parameters[state.strand];

    uint32_t n_kmers = consensus.size() - K + 1;
    uint32_t n_states = n_kmers + 2;
    uint32_t terminal_state = n_states - 1;

    assert(matrix.n_rows == n_states && matrix.n_cols == n_states);

    // Initialize the transition matrix to -INFINITY for all states
    for(size_t si = 0; si < n_states; ++si)
        for(size_t sj = 0; sj < n_states; ++sj)
            set(matrix, si, sj, -INFINITY);

    // Start state transitions -- only allowed to go to k_0
    set(matrix, 0, 1, 0.0f);
    
    // TODO: calculate in log space
    for(size_t si = 1; si < n_states - 1; si++) {
        size_t ki = si - 1;
        double sum = 0.0f;

        uint32_t last_valid_state = si + KHMM_MAX_JUMP;
        if(last_valid_state >= terminal_state)
            last_valid_state = terminal_state - 1;

        for(size_t sj = si; sj <= last_valid_state; ++sj) {
            
            size_t kj = sj - 1;
                        
            // transition probability
            double p_i_j = 0.0f;

            if(ki == kj) {
                p_i_j = parameters.self_transition;
            } else {
        
                uint32_t rank_i = get_rank(state, consensus.c_str(), ki);
                uint32_t rank_j = get_rank(state, consensus.c_str(), kj);

                double level_i = (pm.state[rank_i].level_mean + pm.shift) * pm.scale;
                double level_j = (pm.state[rank_j].level_mean + pm.shift) * pm.scale;
                
                double p_skip = get_skip_probability(parameters, level_i, level_j);
                p_i_j = (1 - sum) * (1 - p_skip);
                assert(p_i_j >= 0.0f && p_i_j <= 1.0f);

#ifdef DEBUG_TRANSITION
                printf("\t\t%zu -> %zu %.2lf %.2lf p_skip: %.4lf p: %.2lf\n", ki, kj, level_i, level_j, p_skip, p_i_j);
#endif
            }

            sum += p_i_j;
            set(matrix, si, sj, log(p_i_j));
        }
    }

    // Transition to end state -- only the last k-mer can go to the end state
    // TODO: allow the last k-mer to be skipped??
    set(matrix, n_states - 2, n_states - 1, 0.0f);
}

void initialize_forward_khmm(DoubleMatrix& fm)
{
    // initialize forward calculation
    for(uint32_t si = 0; si < fm.n_cols; si++)
        set(fm, 0, si, -INFINITY);
    for(uint32_t ri = 0; ri < fm.n_rows; ri++)
        set(fm, ri, 0, -INFINITY);

    set(fm, 0, 0, 0.0f); // probability 1 in the start state for the null row
}

// Terminate the forward algorithm by calculating
// the probability of transitioning to the end state
// for all columns and a given row
double forward_khmm_terminate(const DoubleMatrix& fm,
                              const DoubleMatrix& tm,
                              uint32_t row)
{
    double sum = -INFINITY;
    uint32_t tcol = fm.n_cols - 1;
    for(uint32_t sk = 0; sk < fm.n_cols - 1; sk++) {
        // transition probability from state k to state l
        double t_kl = get(tm, sk, tcol);
        double fm_k = get(fm, row, sk);
        sum = add_logs(sum, t_kl + fm_k);
    }
    return sum;
}

double fill_forward_khmm(DoubleMatrix& fm, // forward matrix
                         const DoubleMatrix& tm, //transitions
                         const char* sequence,
                         const HMMConsReadState& state,
                         uint32_t e_start)
{
    PROFILE_FUNC("fill_forward_khmm")

    // Fill in matrix
    for(uint32_t row = 1; row < fm.n_rows; row++) {
        for(uint32_t sl = 1; sl < fm.n_cols - 1; sl++) {

            // cell indices
            //uint32_t c = cell(matrix, row, col);
            //uint32_t event_i = e_start + (row - 1) * state.stride;
            //uint32_t kmer_idx = k_start + col - 1;

            // Sum over states for previous row
            double sum = -INFINITY;

            // Only look back as far as the first state that can jump here
            uint32_t first_possible_state = 0;
            if(sl >= KHMM_MAX_JUMP)
                first_possible_state = sl - KHMM_MAX_JUMP;

            for(uint32_t sk = first_possible_state; sk <= sl; sk++) {

                // transition probability from state k to state l
                double t_kl = get(tm, sk, sl);
                double fm_k = get(fm, row - 1, sk);
                sum = add_logs(sum, t_kl + fm_k);
#ifdef DEBUG_HMM_UPDATE
                printf("\t(%d %d %d) t: %.2lf f: %.2lf s: %.2lf\n", row, sl, sk, t_kl, fm_k, sum);
#endif
            }

            // Emission probability for event i in state sl
            uint32_t event_idx = e_start + (row - 1) * state.stride;
            uint32_t kmer_idx = sl - 1;
            uint32_t rank = get_rank(state, sequence, kmer_idx);
            double lp_e = log_probability_match(*state.read, rank, event_idx, state.strand);
            
            set(fm, row, sl, lp_e + sum);

#ifdef DEBUG_HMM_UPDATE
            printf("(%d %d) ei: %zu ki: %zu\n", row, sl, event_idx, kmer_idx);
            printf("(%d %d) sum: %.2lf lp_e: %.2lf fm: %.2lf\n", row, sl, sum, lp_e, get(fm, row, sl));
#endif
        }
    }

    // terminate by summing the last row and transitioning to end state
    double sum = -INFINITY;
    uint32_t tcol = fm.n_cols - 1;
    uint32_t lrow = fm.n_rows - 1;
    for(uint32_t sk = 0; sk < fm.n_cols - 1; sk++) {

        // transition probability from state k to state l
        double t_kl = get(tm, sk, tcol);
        double fm_k = get(fm, lrow, sk);
        sum = add_logs(sum, t_kl + fm_k);
    }
    return sum;
}

void initialize_backward_khmm(DoubleMatrix& bm, const DoubleMatrix& tm)
{
    // initialize forward calculation
    uint32_t tcol = tm.n_cols - 1;
    uint32_t row = bm.n_rows - 1;

    for(uint32_t si = 0; si < bm.n_cols; si++)
        set(bm, row, si, get(tm, si, tcol));
}

void fill_backward_khmm(DoubleMatrix& bm, // backward matrix
                         const DoubleMatrix& tm, //transitions
                         const char* sequence,
                         const HMMConsReadState& state,
                         uint32_t e_start)
{
    // Fill in matrix
    for(uint32_t row = bm.n_rows - 2; row > 0; row--) {
        for(uint32_t sk = 1; sk < bm.n_cols - 1; sk++) {

            // Sum over states for next row
            double sum = -INFINITY;
            for(uint32_t sl = 1; sl < bm.n_cols - 1; sl++) {

                // transition probability from state k to state l
                double t_kl = get(tm, sk, sl);
                double bm_l = get(bm, row + 1, sl);

                // Emit E_(i+1) in state sl
                uint32_t event_idx = e_start + row * state.stride; // for i + 1
                uint32_t kmer_idx = sl - 1;
                uint32_t rank = get_rank(state, sequence, kmer_idx);
                double lp_e = log_probability_match(*state.read, rank, event_idx, state.strand);

                sum = add_logs(sum, lp_e + t_kl + bm_l);
#ifdef DEBUG_HMM_UPDATE
                printf("\t(%d %d %d) t: %.2lf b: %.2lf e: %.2lf s: %.2lf\n", row, sk, sl, t_kl, bm_l, lp_e, sum);
#endif
            }
            
            set(bm, row, sk, sum);

#ifdef DEBUG_HMM_UPDATE
            printf("(%d %d) bm: %.2lf\n", row, sk, get(bm, row, sk));
#endif
        }
    }
}

double score_khmm_model(const std::string& consensus, const HMMConsReadState& state, AlignmentPolicy policy)
{
    uint32_t n_kmers = consensus.size() - K + 1;
    uint32_t n_states = n_kmers + 2; // one start and one end state

    DoubleMatrix tm;
    allocate_matrix(tm, n_states, n_states);

    fill_khmm_transitions(tm, consensus, state);
    
    uint32_t e_start = state.event_start_idx;
    uint32_t e_end = state.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;

    uint32_t n_rows = n_events + 1;

    // Allocate a matrix to hold the HMM result
    DoubleMatrix fm;
    allocate_matrix(fm, n_rows, n_states);

    initialize_forward_khmm(fm);
    fill_forward_khmm(fm, tm, consensus.c_str(), state, e_start);

    double score = 0.0f;
    if(policy == AP_GLOBAL) {
        // score by the bottom-right cell
        uint32_t last_row = fm.n_rows - 1;
        score = forward_khmm_terminate(fm, tm, last_row);
    } else if(policy == AP_SEMI_KMER) {

        // score by the best cell in the last column
        double best_score = -INFINITY;
        uint32_t best_row = 0;
        for(size_t row = 1; row < fm.n_rows - 1; ++row) {
            double s = forward_khmm_terminate(fm, tm, row);
            if(s > best_score) {
                best_score = s;
                best_row = row;
            }
        }

        score = best_score;
    } else {
        assert(false);
    }

    free_matrix(tm);
    free_matrix(fm);
    return score;
}


std::vector<PosteriorState> posterior_decode_khmm(const std::string& sequence, const HMMConsReadState& state)
{
    uint32_t n_kmers = sequence.size() - K + 1;
    uint32_t n_states = n_kmers + 2; // one start and one end state

    DoubleMatrix tm;
    allocate_matrix(tm, n_states, n_states);

    fill_khmm_transitions(tm, sequence, state);
    
    uint32_t e_start = state.event_start_idx;
    uint32_t e_end = state.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;

    uint32_t n_rows = n_events + 1;

    // Allocate and compute forward matrix
    DoubleMatrix fm;
    allocate_matrix(fm, n_rows, n_states);

    initialize_forward_khmm(fm);
    double lf = fill_forward_khmm(fm, tm, sequence.c_str(), state, e_start);

    // Allocate and compute backward matrix
    DoubleMatrix bm;
    allocate_matrix(bm, n_rows, n_states);

    initialize_backward_khmm(bm, tm);
    fill_backward_khmm(bm, tm, sequence.c_str(), state, e_start);

    // posterior decode
    std::vector<PosteriorState> output;
    
    uint32_t row = fm.n_rows - 1;
    uint32_t col = fm.n_cols - 1;

    while(row > 0) {

        // Calculate posterior probability that e_i is matched to k_j
        double max_posterior = -INFINITY;
        uint32_t max_s = 0;

        // Only check states that are possible to transition given the previous match col
        uint32_t first_possible_col = 1;
        if(col >= KHMM_MAX_JUMP)
            first_possible_col = col - KHMM_MAX_JUMP;
        
        for(uint32_t si = first_possible_col; si <= col; ++si) {
            double lp = get(fm, row, si) + get(bm, row, si) - lf;
            if(lp > max_posterior) {
                max_posterior = lp;
                max_s = si;
            }
        }
    
        uint32_t event_idx = e_start + (row - 1) * state.stride;
        uint32_t kmer_idx = max_s - 1;
 
        double lpfm = get(fm, row, max_s);

        PosteriorState ps = { event_idx, kmer_idx, max_posterior, lpfm, 0.0f, 'N' };
        output.push_back(ps);

        //
        row -= 1;
        col = max_s;
    }

    std::reverse(output.begin(), output.end());

    // First state is always a match
    output[0].state = 'M';
    uint32_t prev_ei = output[0].event_idx;
    uint32_t prev_ki = output[0].kmer_idx;

    // store the transition probability to this state
    // the + 1 is to convert a k-mer index to a column
    output[0].log_transition_probability = get(tm, 0, output[0].kmer_idx + 1);

    for(uint32_t pi = 1; pi < output.size(); ++pi) {
        uint32_t ei = output[pi].event_idx;
        uint32_t ki = output[pi].kmer_idx;

        output[pi].log_transition_probability = get(tm, prev_ki + 1, ki + 1);
        assert(abs(ei - prev_ei) == 1);

        if(ki == prev_ki) {
            output[pi].state = 'E';
        } else if(ki - prev_ki == 1) {
            output[pi].state = 'M';
        } else {
            assert(ki - prev_ki > 1);
            output[pi].state = 'K';
        }

        prev_ei = ei;
        prev_ki = ki;
    }

    free_matrix(tm);
    free_matrix(fm);
    free_matrix(bm);
    return output;
}

double score_khmm_model_postmerge(const std::string& consensus, const HMMConsReadState& state)
{
    std::vector<PosteriorState> decode = posterior_decode_khmm(consensus, state);
    double lp = 0.0f;
    uint32_t di = 0;
    while(di < decode.size()) {
        
        // Get the range of events that are aligned to the current k-mer
        uint32_t ki = decode[di].kmer_idx;

        uint32_t start = di;

        while(di < decode.size() && decode[di].kmer_idx == ki) {
            di += 1;
        }

        // end is now one past the last event aligned to this kmer
        for(uint32_t i = start; i < di; ++i)
            lp += decode[i].log_transition_probability;

        // sum the event emissions and transitions

        /* single model
        for(uint32_t i = start; i < di; ++i) {
            
            uint32_t ei = decode[i].event_idx;
            uint32_t rank = get_rank(state, consensus.c_str(), ki);
            double lp_e = log_probability_match(*state.read, rank, ei, state.strand);

            //double lp_r_e = log_probability_range_match(*state.read, rank, start_event, end_event, state.stride, state.strand);
            lp += lp_e;
        }
        */

        // multi model
        uint32_t start_event = decode[start].event_idx;
        uint32_t end_event = decode[di - 1].event_idx;
        uint32_t rank = get_rank(state, consensus.c_str(), ki);

        double lp_e = log_probability_range_match(*state.read, rank, start_event, end_event, state.stride, state.strand);
        lp += lp_e;
        
        // advancing the di index is not necessary
        // as it was advanced above   
    }

    return lp;
}

void update_training_khmm(const std::string& consensus, 
                          const HMMConsReadState& state)
{
    std::vector<PosteriorState> pstates = posterior_decode_khmm(consensus, state);

    const CPoreModel& pm = state.read->pore_model[state.strand];
    TrainingData& training_data = state.read->parameters[state.strand].training_data;
    size_t n_kmers = consensus.size() - K + 1;
    uint32_t strand_idx = get_strand_idx(state);

    for(size_t pi = 0; pi < pstates.size(); ++pi) {

        uint32_t ei = pstates[pi].event_idx;
        uint32_t ki = pstates[pi].kmer_idx;
        char s = pstates[pi].state;
    
        // Record transition observations
        // We do not record observations for merge states as there was no kmer transitions
        // We also do not record observations for the beginning of the matches as the
        // alignment may be poor due to edge effects
        if(pi > 5 && pi < pstates.size() - 5) {
 
            // transition           
            if(s != 'E') {
                uint32_t transition_kmer_from = pstates[pi - 1].kmer_idx;
                uint32_t transition_kmer_to = pstates[pi].kmer_idx;

                // Specially handle skips
                // We only want to record the first k-mer skipped if multiple were skipped
                if(s == 'K') {
                    transition_kmer_from = pstates[pi - 1].kmer_idx;
                    transition_kmer_to = transition_kmer_from + 1;
                }
                
                assert(transition_kmer_from < n_kmers && transition_kmer_to < n_kmers);

                uint32_t rank1 = get_rank(state, consensus.c_str(), transition_kmer_from);
                uint32_t rank2 = get_rank(state, consensus.c_str(), transition_kmer_to);
            
                double ke1 = (pm.state[rank1].level_mean + pm.shift) * pm.scale;
                double ke2 = (pm.state[rank2].level_mean + pm.shift) * pm.scale;

#ifdef PRINT_TRAINING_MESSAGES
                printf("TRAIN_SKIP\t%zu\t%.3lf\t%.3lf\t%c\n", strand_idx, ke1, ke2, s);
#endif
                TransitionObservation to = { ke1, ke2, s };
                training_data.transitions.push_back(to);
            }

            // emission
            double level = get_drift_corrected_level(*state.read, ei, state.strand);
            double sd = state.read->events[state.strand].stdv[ei];
            double start_time = state.read->events[state.strand].time[ei];
            double end_time = state.read->events[state.strand].time[ei + 1];
            if(ki >= n_kmers)
                printf("%zu %zu %zu %zu %.2lf %c\n", pi, ei, ki, n_kmers, pstates[pi].l_fm, s);
            
            assert(ki < n_kmers);
            uint32_t rank = get_rank(state, consensus.c_str(), ki);
        
            double model_m = (pm.state[rank].level_mean + pm.shift) * pm.scale;
            double model_s = pm.state[rank].level_stdv * pm.scale;
            double norm_level = (level - model_m) / model_s;

            if(s == 'M')
                training_data.emissions_for_matches.push_back(norm_level);

#ifdef PRINT_TRAINING_MESSAGES
            printf("TRAIN_EMISSION\t%zu\t%zu\t%.3lf\t%.3lf\t%.3lf\t%.3lf\t%.3lf\t%.3lf\t%c\n", strand_idx, ei, level, sd, model_m, model_s, norm_level, end_time - start_time, s);
#endif
        }

        // summary
        training_data.n_matches += (s == 'M');
        training_data.n_merges += (s == 'E');
        training_data.n_skips += (s == 'K');
    }
}

void debug_khmm_model(const std::string& name,
                      uint32_t seq_id,
                      uint32_t read_id,
                      const std::string& consensus, 
                      const HMMConsReadState& state)
{
    std::vector<PosteriorState> pstates = posterior_decode_khmm(consensus, state);
    size_t n_matches = 0;
    size_t n_merges = 0;
    size_t n_skips = 0;
    size_t n_mergeskips = 0;
    
    char prev_s = '\0';
    for(size_t pi = 0; pi < pstates.size(); ++pi) {

        uint32_t ei = pstates[pi].event_idx;
        uint32_t ki = pstates[pi].kmer_idx;
        char s = pstates[pi].state;
    
        double level = get_drift_corrected_level(*state.read, ei, state.strand);
        double sd = state.read->events[state.strand].stdv[ei];
        double duration = get_duration(*state.read, ei, state.strand);
        uint32_t rank = get_rank(state, consensus.c_str(), ki);
        
        const CPoreModel& pm = state.read->pore_model[state.strand];
        double model_m = (pm.state[rank].level_mean + pm.shift) * pm.scale;
        double model_s = pm.state[rank].level_stdv * pm.scale;
        double norm_level = (level - model_m) / model_s;
        
        double model_sd_mean = pm.state[rank].sd_mean;
        double model_sd_stdv = pm.state[rank].sd_stdv;

        n_matches += (s == 'M');
        n_merges += (s == 'E');
        n_skips += (s == 'K');
        n_mergeskips += (s == 'K' && prev_s == 'E');

        double lp_diff = 0.0f;
        if(pi > 0) {
            lp_diff = pstates[pi].l_fm - pstates[pi - 1].l_fm;
        } else {
            lp_diff = pstates[pi].l_fm;
        }
        std::string kmer = consensus.substr(ki, K);
 
        printf("DEBUG\t%s\t%d\t%d\t%c\t", name.c_str(), read_id, state.rc, state.strand ? 't' : 'c');
        printf("%c\t%zu\t%zu\t", s, ei, ki);
        printf("%s\t%.3lf\t", kmer.c_str(), duration);
        printf("%.1lf\t%.1lf\t%.1lf\t", level, model_m, norm_level);
        printf("\t%.1lf\t%.1lf\t%.1lf\t", sd, model_sd_mean, (sd - model_sd_mean) / model_sd_stdv);
        printf("%.2lf\t%.2lf\t%.2lf\n", exp(pstates[pi].l_posterior), pstates[pi].l_fm, lp_diff);
        prev_s = s;
    }

    // Summarize alignment
    double time_start = state.read->events[state.strand].time[state.event_start_idx];
    double time_end = state.read->events[state.strand].time[state.event_stop_idx];
    double total_duration = fabs(time_start - time_end);
    double num_events = abs(state.event_start_idx - state.event_stop_idx) + 1;
    double final_lp = pstates[pstates.size() - 1].l_fm;
    double mean_lp = final_lp / num_events;

    // Print summary header on first entry
    static int once = 1;
    if(once) {
        printf("SUMMARY\tseq_name\tseq_id\tread_id\tis_rc\tstrand\t");
        printf("lp\tmean_lp\tnum_events\t");
        printf("n_matches\tn_merges\tn_skips\tn_mergeskips\ttotal_duration\n");
        once = 0;
    }

    printf("SUMMARY\t%s\t%d\t%d\t%d\t%c\t", name.c_str(), seq_id, read_id, state.rc, state.strand ? 't' : 'c');
    printf("%.2lf\t%.2lf\t%.0lf\t", final_lp, mean_lp, num_events);
    printf("%d\t%d\t%d\t%d\t%.2lf\n", n_matches, n_merges, n_skips, n_mergeskips, total_duration);
}

//
// NEW MODEL
//
double fill_viterbi_skip_merge(DoubleMatrix& m, // score matrix
                               const DoubleMatrix& tm, //transitions
                               const char* sequence,
                               const HMMConsReadState& state,
                               uint32_t e_start)
{
    PROFILE_FUNC("fill_viterbi_skip_merge")

    // Fill in matrix
    for(uint32_t row = 1; row < m.n_rows; row++) {
        for(uint32_t col = 1; col < m.n_cols - 1; col++) {

#ifdef DEBUG_HMM_UPDATE
            printf("[%d %d]\n", row, col);
#endif

            double max = -INFINITY;
            
            uint32_t first_possible_row = 1;
            if(row > KHMM_MAX_MERGE)
                first_possible_row = row - KHMM_MAX_MERGE;
            
            uint32_t first_possible_col = 0;
            if(col >= KHMM_MAX_JUMP)
                first_possible_col = col - KHMM_MAX_JUMP;
            
            // Calculate probability of matching starting at particular row/col
            // for all the possible paths to here
            for(uint32_t start_row = first_possible_row; start_row <= row; ++start_row) {
                for(uint32_t start_col = first_possible_col; start_col < col; ++start_col) {
                    
                    // score for sr - 1, sc
                    double m_prev = get(m, start_row - 1, start_col);
                
                    // The score of emitting a range of events in this column
                    uint32_t start_event = e_start + (start_row - 1) * state.stride;
                    uint32_t end_event = e_start + (row - 1) * state.stride;
            
                    uint32_t kmer_idx = col - 1;
                    uint32_t rank = get_rank(state, sequence, kmer_idx);
                    
                    double lp_r_e = log_probability_range_match(*state.read, rank, start_event, end_event, state.stride, state.strand);

                    // probability of transition into this column from start_col
                    double t_jump = get(tm, start_col, col);

                    // probability of staying in this column n times
                    uint32_t n_merges = row - start_row;
                    double t_merge = n_merges * get(tm, col, col);

                    double total = m_prev + lp_r_e + t_jump + t_merge;
#ifdef DEBUG_HMM_UPDATE
                    printf("\tstart: [%d %d] e: [%d %d] lp: %.2lf t_jump: %.2lf t_merge: %.2lf t: %.2lf\n", start_row, start_col, start_event, end_event, lp_r_e, t_jump, t_merge, total);
#endif 
                    if(total > max)
                        max = total;
                }
            }
            set(m, row, col, max);
        }
    }

    // terminate by returning max in the last row
    uint32_t tcol = m.n_cols - 1;
    uint32_t lrow = m.n_rows - 1;
    double max = -INFINITY;
    for(uint32_t col = 0; col < m.n_cols - 1; ++col) {

        // transition probability from state k to state l
        double t_kl = get(tm, col, tcol);
        double m_k = get(m, lrow, col);
        double total = t_kl + m_k;
        if(total > max)
            max = total;
    }
    return max;
}


double score_skip_merge(const std::string& consensus, const HMMConsReadState& state)
{
    uint32_t n_kmers = consensus.size() - K + 1;
    uint32_t n_states = n_kmers + 2; // one start and one end state

    DoubleMatrix tm;
    allocate_matrix(tm, n_states, n_states);

    fill_khmm_transitions(tm, consensus, state);
    
    uint32_t e_start = state.event_start_idx;
    uint32_t e_end = state.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;

    uint32_t n_rows = n_events + 1;

    // Allocate a matrix to hold the HMM result
    DoubleMatrix fm;
    allocate_matrix(fm, n_rows, n_states);

    initialize_forward_khmm(fm);
    double score = fill_viterbi_skip_merge(fm, tm, consensus.c_str(), state, e_start);

    free_matrix(tm);
    free_matrix(fm);
    return score;
}

double score_emission_dp(const std::string& sequence, const HMMConsReadState& state)
{
    uint32_t n_kmers = sequence.size() - K + 1;
    uint32_t n_cols = n_kmers + 1;

    uint32_t e_start = state.event_start_idx;
    uint32_t e_end = state.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;

    uint32_t n_rows = n_events + 1;

    // Allocate a matrix to hold the HMM result
    DoubleMatrix m;
    allocate_matrix(m, n_rows, n_cols);

    // Initialize matrix to -INFINITY except for (0,0)
    for(uint32_t row = 0; row < m.n_rows; ++row) {
        for(uint32_t col = 0; col < m.n_cols; ++col) {
            set(m, row, col, -INFINITY);
        }
    }
    set(m, 0, 0, 0.0f);

    // Fill matrix
    for(uint32_t row = 1; row < m.n_rows; ++row) {
        for(uint32_t col = 1; col < m.n_cols; ++col) {
            
            // Get the emission probability for matching this event to this kmer
            uint32_t event_idx = e_start + (row - 1) * state.stride;
            uint32_t kmer_idx = col - 1;
            uint32_t rank = get_rank(state, sequence.c_str(), kmer_idx);
            double lp_e = log_probability_match(*state.read, rank, event_idx, state.strand);
            
            // Get the scores for the 3 possible preceding cells           
            double up = lp_e + get(m, row - 1, col);
            double diag = lp_e + get(m, row - 1, col - 1);
            double left = get(m, row, col - 1);
            double best = max3(up, diag, left);

            set(m, row, col, best);
        }
    }

    double score = get(m, m.n_rows - 1, m.n_cols - 1);
    free_matrix(m);
    return score;
}

// The indices of a k-mer match in a pair of sequences
struct kLCSPair
{
    uint32_t i;
    uint32_t j;
};
typedef std::vector<kLCSPair> kLCSResult;

// Helper to backtrack through the kLCS matrix
void _kLCSBacktrack(const UInt32Matrix& m,
                    const std::string& a, 
                    const std::string& b,
                    uint32_t row,
                    uint32_t col,
                    kLCSResult& result)
{
    if(row == 0 || col == 0)
        return;

    const char* ka = a.c_str() + row - 1;
    const char* kb = b.c_str() + col - 1;

    if(strncmp(ka, kb, K) == 0) {
        kLCSPair p = { row - 1, col - 1 };
        result.push_back(p);
        return _kLCSBacktrack(m, a, b, row - 1, col - 1, result);
    } else {

        if(get(m, row - 1, col) > get(m, row, col - 1)) {
            return _kLCSBacktrack(m, a, b, row - 1, col, result);
        } else {
            return _kLCSBacktrack(m, a, b, row, col - 1, result);
        }
    }
}

// Return the longest common subseuqence of k-mers between the two strings
kLCSResult kLCS(const std::string& a, const std::string& b)
{
    uint32_t n_kmers_a = a.size() - K + 1;
    uint32_t n_kmers_b = b.size() - K + 1;

    uint32_t n_rows = n_kmers_a + 1;
    uint32_t n_cols = n_kmers_b + 1;

    UInt32Matrix m;
    allocate_matrix(m, n_rows, n_cols);

    // Initialize first row/col to zero
    for(uint32_t row = 0; row < m.n_rows; ++row)
        set(m, row, 0, 0);
    for(uint32_t col = 0; col < m.n_cols; ++col)
        set(m, 0, col, 0);
    
    // Fill matrix
    for(uint32_t row = 1; row < m.n_rows; ++row) {
        for(uint32_t col = 1; col < m.n_cols; ++col) {
    
            const char* ka = a.c_str() + row - 1;
            const char* kb = b.c_str() + col - 1;

            uint32_t score = 0;
            if(strncmp(ka, kb, K) == 0) {
                uint32_t diag = get(m, row - 1, col - 1);
                score = diag + 1;
            } else {
                uint32_t left = get(m, row, col - 1);
                uint32_t up = get(m, row - 1, col);
                score = std::max(left, up);
            }
            set(m, row, col, score);
        }
    }

    kLCSResult result;
    _kLCSBacktrack(m, a, b, n_rows - 1, n_cols -  1, result);

    // Backtrack appends from the end to the start, reverse the vector of matches
    std::reverse(result.begin(), result.end());
    free_matrix(m);
    return result;
}

// Handy wrappers for scoring/debugging functions
// The consensus algorithms call into these so we can switch
// scoring functinos without writing a bunch of code
double score_sequence(const std::string& sequence, const HMMConsReadState& state)
{
    //return score_skip_merge(sequence, state);
    //return score_khmm_model_postmerge(sequence, state);
    return score_khmm_model(sequence, state, AP_GLOBAL);
    //return score_emission_dp(sequence, state);
}

void debug_sequence(const std::string& name, uint32_t seq_id, uint32_t read_id, const std::string& sequence, const HMMConsReadState& state)
{
    return debug_khmm_model(name, seq_id, read_id, sequence, state);
}

std::vector<PosteriorState> posterior_decode(const std::string& sequence, const HMMConsReadState& state)
{
    return posterior_decode_khmm(sequence, state);
}

struct PathCons
{
    // default constructor
    PathCons(const std::string& s) : path(s), score(0.0f), sum_rank(0) {}

    std::string path;
    
    double score;
    size_t sum_rank;
    size_t num_improved;
    size_t num_scored;
    
    std::string mutdesc;
    
};
typedef std::vector<PathCons> PathConsVector;

bool sortPathConsScoreDesc(const PathCons& a, const PathCons& b)
{
    return a.score > b.score;
}

bool sortPathConsRankAsc(const PathCons& a, const PathCons& b)
{
    return a.sum_rank < b.sum_rank;
}

bool sortPathConsRankDesc(const PathCons& a, const PathCons& b)
{
    return a.sum_rank > b.sum_rank;
}

struct IndexedPathScore
{
    double score;
    uint32_t path_index;
};

bool sortIndexedPathScoreDesc(const IndexedPathScore& a, const IndexedPathScore& b)
{
    return a.score > b.score;
}

// This scores each path using the HMM and 
// sorts the paths into ascending order by score
void score_paths(PathConsVector& paths, const std::vector<HMMConsReadState>& read_states)
{
    PROFILE_FUNC("score_paths")
    double MIN_FIT = INFINITY;
    size_t CULL_RATE = 5;
    double CULL_MIN_SCORE = -30.0f;
    double CULL_MIN_IMPROVED_FRACTION = 0.2f;

    // cache the initial sequence
    std::string first = paths[0].path;
    
    PathConsVector dedup_paths;

    // initialize and deduplicate paths to avoid redundant computation
    std::set<std::string> path_string_set;
    for(size_t pi = 0; pi < paths.size(); ++pi) {

        if(path_string_set.find(paths[pi].path) == path_string_set.end()) {
            paths[pi].score = 0;
            paths[pi].sum_rank = 0;
            paths[pi].num_improved = 0;
            paths[pi].num_scored = 0;
            dedup_paths.push_back(paths[pi]);
            path_string_set.insert(paths[pi].path);
        }
    }
    paths.clear();
    paths.swap(dedup_paths);
    

    // Score all reads
    for(uint32_t ri = 0; ri < read_states.size(); ++ri) {
        printf("Scoring %d\n", ri);

        const HMMConsReadState& read_state = read_states[ri];
        const KHMMParameters& parameters = read_state.read->parameters[read_state.strand];
 
        if( fabs(parameters.fit_quality) > MIN_FIT)
            continue;

        std::vector<IndexedPathScore> result(paths.size());

        // Score all paths
        omp_set_num_threads(g_data.num_threads);
        #pragma omp parallel for
        for(size_t pi = 0; pi < paths.size(); ++pi) {
            double curr = score_sequence(paths[pi].path, read_states[ri]);
            result[pi].score = curr;
            result[pi].path_index = pi;
        }

        // Save score of first path
        double first_path_score = result[0].score;

        // Sort result by score
        std::stable_sort(result.begin(), result.end(), sortIndexedPathScoreDesc);

        for(size_t pri = 0; pri < result.size(); ++pri) {
            size_t pi = result[pri].path_index;

            paths[pi].score += (result[pri].score - first_path_score);
            uint32_t rank_score = pri;
            paths[pi].sum_rank += rank_score;
            paths[pi].num_improved += (result[pri].score > first_path_score);
            paths[pi].num_scored += 1;
        }

        // Cull paths
        if(ri > 0 && ri % CULL_RATE == 0) {
            PathConsVector retained_paths;
            for(size_t pi = 0; pi < paths.size(); ++pi) {
                
                // We keep a path if any of these conditions are met:
                //  1) it is the original unmodified sequence
                //  2) its score is greater than CULL_MIN_SCORE
                //  3) the fraction of reads that score better on this
                //     path compared to the original sequence is greater
                //     than CULL_MIN_IMPROVED_FRACTION
                double f = (double)paths[pi].num_improved / (double)paths[pi].num_scored;
                if(pi == 0 || paths[pi].score > CULL_MIN_SCORE || f >= CULL_MIN_IMPROVED_FRACTION) {
                    retained_paths.push_back(paths[pi]);
                }
            }
            paths.swap(retained_paths);
        }
    }

    // select new sequence
    //std::stable_sort(paths.begin(), paths.end(), sortPathConsRankAsc);
    std::stable_sort(paths.begin(), paths.end(), sortPathConsScoreDesc);

#if DEBUG_PATH_SELECTION
    for(size_t pi = 0; pi < paths.size(); ++pi) {

        // Calculate the length of the matching prefix with the initial sequence
        const std::string& s = paths[pi].path;

        char initial = s == first ? 'I' : ' ';

        printf("%zu\t%s\t%.1lf\t%zu %c %s", pi, paths[pi].path.c_str(), paths[pi].score, paths[pi].sum_rank, initial, paths[pi].mutdesc.c_str());
        // If this is the truth path or the best path, show the scores for all reads
        if(pi == 0 || initial == 'I') {
            for(uint32_t ri = 0; ri < read_states.size(); ++ri) {
                const HMMConsReadState& read_state = read_states[ri];
                const KHMMParameters& parameters = read_state.read->parameters[read_state.strand];
                if( fabs(parameters.fit_quality) > MIN_FIT)
                    continue;

                double curr = score_sequence(paths[pi].path, read_states[ri]);
                printf("%.1lf,%.2lf ", parameters.fit_quality, curr);
            }
        }
        printf("\n");
    }
#endif

}

void extend_paths(PathConsVector& paths, int maxk = 2)
{
    // Insert all possible extensions into the path sequence
    // for k in 1 to maxk
    PathConsVector new_paths;

    for(int k = 1; k <= maxk; ++k) {

        for(int pi = 0; pi < paths.size(); ++pi) {
    
            std::string first(k, 'A');
            std::string extension = first;

            do {
                std::string current = paths[pi].path;
                std::string ns = current.insert(current.size() - 5, extension);
                PathCons ps(ns);
                new_paths.push_back(ps);
                lexicographic_next(extension);
            } while(extension != first);
        }
    }

    paths.swap(new_paths);
}

PathConsVector generate_mutations(const std::string& sequence)
{
    PathConsVector mutations;

    // Add the unmutated sequence
    {
        PathCons pc(sequence);
        mutations.push_back(pc);
    }

    // Mutate every base except for in the first/last k-mer
    for(size_t si = K; si < sequence.size() - K; ++si) {
        
        // All subs
        for(size_t bi = 0; bi < 4; bi++) {
            char b = "ACGT"[bi];
            if(sequence[si] == b)
                continue;
            PathCons pc(sequence);
            pc.path[si] = b;
            std::stringstream ss;
            ss << "sub-" << si << "-" << b;
            pc.mutdesc = ss.str();
            mutations.push_back(pc);
        }

        // 1bp del at this position
        {
            PathCons pc(sequence);
            pc.path.erase(si, 1);
            
            std::stringstream ss;
            ss << "del-" << si;
            pc.mutdesc = ss.str();
            
            mutations.push_back(pc);
        }

        // All 1bp ins before this position
        for(size_t bi = 0; bi < 4; bi++) {
            char b = "ACGT"[bi];
            PathCons pc(sequence);
            pc.path.insert(si, 1, b);
            
            std::stringstream ss;
            ss << "ins-" << si << "-" << b;
            pc.mutdesc = ss.str();
            
            mutations.push_back(pc);
        }
    }

    return mutations;
}

void run_mutation(std::string& base, const std::vector<HMMConsReadState>& read_states, std::string& second_best)
{
    PROFILE_FUNC("run_mutation")
    int iteration = 0;
    while(iteration++ < 10) {

        // Generate possible sequences
        PathConsVector paths = generate_mutations(base);

        score_paths(paths, read_states);

        if(paths.size() > 1) {
            second_best = paths[1].path;
        } else {
            second_best = "";   
        }

        // check if no improvement was made
        if(paths[0].path == base)
            break;
        base = paths[0].path;
    }
}

void generate_alt_paths(PathConsVector& paths, const std::string& base, const std::vector<std::string>& alts)
{
    // Generate alternatives
    for(uint32_t ai = 0; ai < alts.size(); ++ai) {
        const std::string& alt = alts[ai];
        kLCSResult result = kLCS(base, alt);

#ifdef DEBUG_ALT_GENERATION
        printf("Match to alt %s\n", alt.c_str());
        for(size_t mi = 0; mi < result.size(); ++mi) {
            std::string extend = "";
            if(mi < result.size() - 1 && result[mi].j + 1 != result[mi + 1].j) {
                extend = alt.substr(result[mi].j, result[mi + 1].j - result[mi].j + K);
            }
            printf("\t%zu %zu %s %s\n", result[mi].i, result[mi].j, base.substr(result[mi].i, K).c_str(), extend.c_str());
        }
#endif

        uint32_t match_idx = 0;
        while(match_idx < result.size()) {
            uint32_t last_idx = result.size() - 1;

            // advance the match to the next point of divergence
            while(match_idx != last_idx && 
                  result[match_idx].i == result[match_idx + 1].i - 1 &&
                  result[match_idx].j == result[match_idx + 1].j - 1) {
                match_idx++;
            }
            // no more divergences to process
            if(match_idx == last_idx)
                break;

            uint32_t bl = result[match_idx + 1].i - result[match_idx].i;
            uint32_t rl = result[match_idx + 1].j - result[match_idx].j;

            std::string base_subseq = base.substr(result[match_idx].i, bl);
            std::string alt_subseq = alt.substr(result[match_idx].j, rl);
            
            // Perform the splice
            PathCons new_path(base);
            new_path.path.replace(result[match_idx].i, bl, alt_subseq);
            paths.push_back(new_path);
            
            match_idx += 1;
        }
    }
}

//
// Outlier filtering
//
void filter_outlier_read_states(std::vector<HMMConsReadState>& read_states, const std::string& sequence)
{
    std::vector<HMMConsReadState> out_rs;
    for(uint32_t ri = 0; ri < read_states.size(); ++ri) {
        const HMMConsReadState& rs = read_states[ri];

        double curr = score_sequence(sequence, rs);
        double n_events = abs(rs.event_start_idx - rs.event_stop_idx) + 1.0f;
        double lp_per_event = curr / n_events;
        printf("OUTLIER_FILTER %zu %.2lf %.2lf %.2lf\n", ri, curr, n_events, lp_per_event);
        if(fabs(lp_per_event) < 3.5f) {
            out_rs.push_back(rs);
        }
    }
    read_states.swap(out_rs);
}

std::string join_sequences_at_kmer(const std::string& a, const std::string& b)
{
    // These sequences must have a k-mer match at the start/end
    std::string a_last_kmer = a.substr(a.size() - K);
    std::string b_last_kmer = b.substr(0, K);
    assert(a_last_kmer == b_last_kmer);
    return a + b.substr(K);
}

void run_splice_segment(uint32_t segment_id)
{
    if(!g_initialized) {
        printf("ERROR: initialize() not called\n");
        exit(EXIT_FAILURE);
    }

    // The structure of the data looks like this:

    // --------------------------------------------------------
    // S                       M                              E
    // where is the start column, M is the middle column and E
    // is the end column. We want to call a new consensus from S
    // to E. We do this by generating the base sequence from S to E
    // and then applying all of the alternatives indicated by the
    // start and middle column. We score these alternatives using
    // the read strands spanning from S to E. After a new consensus
    // has been selected, we re-calculate the alignments of events to
    // the middle anchor.

    // Get the segments
    assert(segment_id + 2 < g_data.anchored_columns.size());
    HMMAnchoredColumn& start_column = g_data.anchored_columns[segment_id];
    HMMAnchoredColumn& middle_column = g_data.anchored_columns[segment_id + 1];
    HMMAnchoredColumn& end_column = g_data.anchored_columns[segment_id + 2];

    std::string s_m_base = start_column.base_sequence;
    std::string m_e_base = middle_column.base_sequence;

    // The current consensus sequence
    std::string original = join_sequences_at_kmer(s_m_base, m_e_base);
    std::string base = original;
    
    // The collection of alternative sequences
    std::vector<std::string> alts;

    for(uint32_t ai = 0; ai < start_column.alt_sequences.size(); ++ai) {
        // first segment alts plus the base of the middle segment
        if(m_e_base.length() >= K)
            alts.push_back(start_column.alt_sequences[ai] + m_e_base.substr(K));
    }
    
    for(uint32_t ai = 0; ai < middle_column.alt_sequences.size(); ++ai) {
        // base first segment plus alts of middle segment
        if(middle_column.alt_sequences[ai].length() >= K)
            alts.push_back(s_m_base.substr(0, s_m_base.size() - K) + middle_column.alt_sequences[ai]);
    }

    // Set up the HMMReadStates, which are used to calculate
    // the probability of the data given a possible consensus sequence
    std::vector<HMMConsReadState> read_states = get_read_states_for_columns(start_column, end_column);

    //
    filter_outlier_read_states(read_states, base);

    // Only attempt correction if there are any reads here
    if(!read_states.empty()) {
        uint32_t num_rounds = 6;
        uint32_t round = 0;
        while(round++ < num_rounds) {
            
            PathConsVector paths;
            PathCons base_path(base);
            paths.push_back(base_path);
            
            generate_alt_paths(paths, base, alts);
            score_paths(paths, read_states);

            if(paths[0].path == base)
                break;
            base = paths[0].path;
        }

        std::string second_best;
        run_mutation(base, read_states, second_best);

#if DEBUG_SHOW_TOP_TWO
        if(!second_best.empty()) {
            for(uint32_t ri = 0; ri < read_states.size(); ++ri) {
                debug_sequence("best", segment_id, ri, base, read_states[ri]);
                debug_sequence("second", segment_id, ri, second_best, read_states[ri]);
            }
        }
#endif
    }

    printf("ORIGINAL[%zu] %s\n", segment_id, original.c_str());
    printf("RESULT[%zu]   %s\n", segment_id, base.c_str());

    // Update the sequences for the start and middle segments
    // by cutting the new consensus in the middle
    // We maintain the k-mer match invariant by requiring the
    // sequences to overlap by 5bp
    assert(base.length() >= K);
    uint32_t midpoint_kmer = (base.length() - K + 1) / 2;

    std::string s_m_fixed = base.substr(0, midpoint_kmer + K);
    std::string m_e_fixed = base.substr(midpoint_kmer);

    assert(s_m_fixed.substr(s_m_fixed.size() - K) == m_e_fixed.substr(0, K));

    start_column.base_sequence = s_m_fixed;
    middle_column.base_sequence = m_e_fixed;

    // Update the event indices in the first column to match 
    for(uint32_t ri = 0; ri < read_states.size(); ++ri) {

        // Realign to the consensus sequence
        std::vector<PosteriorState> decodes = posterior_decode(base, read_states[ri]);

        // Get the closest event aligned to the target kmer
        int32_t min_k_dist = base.length();
        uint32_t event_idx = 0;
        for(uint32_t di = 0; di < decodes.size(); ++di) {
            int32_t dist = abs(decodes[di].kmer_idx - midpoint_kmer);
            if(dist <= min_k_dist) {
                min_k_dist = dist;
                event_idx = decodes[di].event_idx;
            }
        }

        middle_column.anchors[read_states[ri].anchor_index].event_idx = event_idx;
    }
}

extern "C"
void run_splice()
{
    if(!g_initialized) {
        printf("ERROR: initialize() not called\n");
        exit(EXIT_FAILURE);
    }
 
    std::string uncorrected = "";
    std::string consensus = "";

    uint32_t start_segment_id = 0;
#ifdef DEBUG_SINGLE_SEGMENT
    start_segment_id = 118;
#endif

    uint32_t num_segments = g_data.anchored_columns.size();
    for(uint32_t segment_id = start_segment_id; segment_id < num_segments - 2; ++segment_id) {

        // Track the original sequence for reference
        if(uncorrected.empty()) {
            uncorrected = g_data.anchored_columns[segment_id].base_sequence;
        } else {
            uncorrected.append(g_data.anchored_columns[segment_id].base_sequence.substr(K));
        }

        // run the consensus algorithm for this segment
        run_splice_segment(segment_id);

        // run_splice_segment updates the base_sequence of the current anchor, grab it and append
        std::string base = g_data.anchored_columns[segment_id].base_sequence;

        if(consensus.empty()) {
            consensus = base;
        } else {
            // The first 5 bases of the incoming sequence must match
            // the last 5 bases of the growing consensus
            // run_splice_segment must ensure this
            assert(consensus.substr(consensus.size() - K) == base.substr(0, K));
            consensus.append(base.substr(K));
        }

        printf("UNCORRECT[%zu]: %s\n", segment_id, uncorrected.c_str());
        printf("CONSENSUS[%zu]: %s\n", segment_id, consensus.c_str());
#ifdef DEBUG_SINGLE_SEGMENT
        break;
#endif
    }

    g_data.consensus_result = consensus;
}

// update the training data on the current segment
extern "C"
void train_segment(uint32_t segment_id)
{
    if(!g_initialized) {
        printf("ERROR: initialize() not called\n");
        exit(EXIT_FAILURE);
    }

    // Get the segments
    assert(segment_id + 2 < g_data.anchored_columns.size());
    HMMAnchoredColumn& start_column = g_data.anchored_columns[segment_id];
    HMMAnchoredColumn& middle_column = g_data.anchored_columns[segment_id + 1];
    HMMAnchoredColumn& end_column = g_data.anchored_columns[segment_id + 2];

    std::string s_m_base = start_column.base_sequence;
    std::string m_e_base = middle_column.base_sequence;

    std::string segment_sequence = join_sequences_at_kmer(s_m_base, m_e_base);

    // Set up the HMMReadStates, which are used to calculate
    // the probability of the data given a possible consensus sequence
    std::vector<HMMConsReadState> read_states = get_read_states_for_columns(start_column, end_column);
     
    for(uint32_t ri = 0; ri < read_states.size(); ++ri) {

        std::vector<PosteriorState> decodes = posterior_decode(segment_sequence, read_states[ri]);
        update_training_khmm(segment_sequence, read_states[ri]);
    }
}

extern "C"
void train()
{
    // train on current consensus
    uint32_t num_segments = g_data.anchored_columns.size();
    for(uint32_t segment_id = 0; segment_id < num_segments - 2; ++segment_id) {
        printf("Training segment %zu\n", segment_id);
        train_segment(segment_id);
    }

    // Update model parameters
    for(uint32_t ri = 0; ri < g_data.reads.size(); ++ri) {
        khmm_parameters_train(g_data.reads[ri].parameters[0]);
        khmm_parameters_train(g_data.reads[ri].parameters[1]);
    }
}

int main(int argc, char** argv)
{

}
