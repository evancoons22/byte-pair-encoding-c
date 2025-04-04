#include <stdio.h> 
#include <stdlib.h> 
#include <math.h>


#define DIMENSION_EMBEDDING 256
#define DIMENSION_HIDDEN 256  // usually the same as the embedding (model) dimension
#define NUM_HEADS 4
#define DIMENSION_KEYS (DIMENSION_HIDDEN / NUM_HEADS)  // 256 / 4 = 64
#define DIMENSION_VALUES (DIMENSION_HIDDEN / NUM_HEADS) // 256 / 4 = 64
#define NN_SIZE 1024  // 4 * DIMENSION_HIDDEN
#define VOCAB_SIZE 500

typedef struct { 
    float* items; 
    size_t count;
    size_t capacity;
} Activations;

typedef struct { 
    float* items; 
    size_t count;
    size_t capacity;
} Weights;

void mha_forward();
void mmha_forward();
void nn_forward();
void linear_forward();
void add_and_norm();
void linear();

#include <math.h>
#include <stdlib.h>

// Compute positional encoding for a given maximum sequence length and model dimension.
// 'pe' should be a pre-allocated array of size max_seq_len * d_model.
void compute_positional_encoding(float *pe, int max_seq_len, int d_model) {
    for (int pos = 0; pos < max_seq_len; pos++) {
        for (int i = 0; i < d_model; i += 2) {
            // calculate the "frequency" for this dimension
            float freq = 1.0 / pow(10000, (float)i / d_model);
            float angle = pos * freq;
            // even indices get sine
            pe[pos * d_model + i] = sin(angle);
            // Make sure we don't go out of bounds
            if (i + 1 < d_model) {
                pe[pos * d_model + i + 1] = cos(angle);
            }
        }
    }
}


void softmax(float input[], float output[], size_t size) {
    float sum = 0; 
    for (size_t i = 0; i < size; i++) { 
        sum += exp(input[i]); 
    } 
    for (size_t i = 0; i < size; i++) { 
        output[i] = exp(input[i]) / sum;
    } 
} 

void softmax_matrix(float input[], float output[], size_t N, size_t M) {
    // Apply softmax row-wise (each row sums to 1)
    for (size_t i = 0; i < N; i++) {
        // Find max value in the row for numerical stability
        float max_val = input[i * M];
        for (size_t j = 1; j < M; j++) {
            if (input[i * M + j] > max_val) {
                max_val = input[i * M + j];
            }
        }
        float sum = 0.0f;
        for (size_t j = 0; j < M; j++) {
            sum += exp(input[i * M + j] - max_val);
        }
        for (size_t j = 0; j < M; j++) {
            output[i * M + j] = exp(input[i * M + j] - max_val) / sum;
        }
    }
}

void transpose(float X[], int N, int M) {
    float *temp = malloc(N * M * sizeof(float));
    for (int i = 0; i < N * M; i++) { temp[i] = X[i]; }
    // Transpose from temp back to X
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            X[j * N + i] = temp[i * M + j];
        }
    }
    free(temp);
}

void matmul(float A[], float B[], int N, int D, int M, float result[]) { 
    // A is N x D, B is D x M, result is N x M
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            result[i * M + j] = 0;
            for (int k = 0; k < D; k++) {
                result[i * M + j] += A[i * D + k] * B[k * M + j];
            }
        }
    }
}

// size is the input size
// Q, K, V have already been projected here to size x d_k
void attention(float Q[], float K[], float V[], int size, float* result) { 
    // Q * K^T
    // copy K into new memory to avoid corruption due to transpose
    float K_temp[size*DIMENSION_KEYS];
    for (int i = 0; i < size*DIMENSION_KEYS; i++) { K_temp[i] = K[i]; }

    transpose(K_temp, size, DIMENSION_KEYS);
    float *QKT = malloc(size * size * sizeof(float));
    matmul(Q, K_temp, size, DIMENSION_KEYS, size, QKT);
    // scale
    float factor = sqrt(DIMENSION_KEYS);
    for (int i = 0; i < size; i ++ ) { 
        for (int j = 0; j < size; j ++ ) { 
            QKT[i * size + j] /= factor;
        } 
    } 
    //softmax
    float *QKT_out = malloc(size * size * sizeof(float));
    softmax_matrix(QKT, QKT_out, size, size);
    matmul(QKT_out, V, size, size, DIMENSION_VALUES, result);

    free(QKT);
    free(QKT_out);
} 

void masked_attention(float Q[], float K[], float V[], int size, float* result) { 
    // Q * K^T
    transpose(K, size, DIMENSION_KEYS);
    float *QKT = malloc(size * size * sizeof(float));
    matmul(Q, K, size, DIMENSION_KEYS, size, QKT);
    // scale
    float factor = sqrt(DIMENSION_KEYS);
    for (int i = 0; i < size; i ++ ) { 
        for (int j = 0; j < size; j ++ ) { 
            QKT[i * size + j] /= factor;
        } 
    } 
    
    // Apply causal mask: set upper triangular part (future tokens) to negative infinity
    // This ensures tokens can only attend to previous tokens and themselves
    for (int i = 0; i < size; i++) {
        for (int j = i + 1; j < size; j++) {
            QKT[i * size + j] = -INFINITY;
        }
    }
    
    //softmax
    float *QKT_out = malloc(size * size * sizeof(float));
    softmax_matrix(QKT, QKT_out, size, size);
    matmul(QKT_out, V, size, size, DIMENSION_VALUES, result);

    free(QKT);
    free(QKT_out);
} 

//rows of Q, K, V are the embeddings
//void attention_projection (const float* input, const float* W_Q, const float* W_K, const float* W_V, float* Q, float* K, float* V, int d_model) { 
//
//}


void nn_forward(float weights[], float activations[], float z[]) { 
    // alignment in memory matters here: 
    // weights index: layer i, to node k from node j
    // input will be considered the first of the activations
    // output will be considered the last of the activation

    // input layer is size 256
    // NN_SIZE 1024
    //     *
    //     *
    // *   *   *
    // *   *   *
    //     *
    //     *

    // first layer
    int layer = 1;
    for(size_t k = 0; k < NN_SIZE; k ++) {
        for(size_t j = 0; j < DIMENSION_VALUES; j ++) { 
            z[layer * 256 + k] += activations[j] * weights[k * DIMENSION_VALUES + j]; // 0 * 256 + j 
        } 
        activations[layer * 256 + k] = z[layer * 256 + k]; // some type of activation here
    } 


    // second layer
    layer = 3;
    for(size_t k = 0; k < DIMENSION_VALUES; k ++) {
        for(size_t j = 0; j < NN_SIZE; j ++) { 
            z[256 + 1024 * 2 + k] += activations[256 + 2 * NN_SIZE + j] * weights[256*1024 + 1024*1024 + k * NN_SIZE + j]; 
        } 
        activations[256 + 1024 * 2 + k] = z[256 + 1024 * 2 + k]; // some type of activation here
    } 
} 

void convert_to_embeddings(float *E, float* embeddings, float* input, int size){ 
    // Create learned embedding matrix E
    // Initialize E with random values (in a real scenario, this would be loaded from a trained model)
    for (int i = 0; i < VOCAB_SIZE * DIMENSION_EMBEDDING; i++) {
        E[i] = ((float)rand() / RAND_MAX) * 0.1f;  // Small random values
    }
    // Allocate memory for embeddings
    
    // Convert tokens to embeddings (skipping actual one-hot encoding for efficiency)
    // In practice, this is equivalent to selecting the corresponding row from E for each token
    for (int i = 0; i < size; i++) {
        int token_id = (int)input[i];
        if (token_id >= VOCAB_SIZE) token_id = 0;  // Handle out-of-vocabulary tokens
        
        // Copy the embedding for this token (equivalent to multiplying one-hot by E)
        for (int j = 0; j < DIMENSION_EMBEDDING; j++) {
            embeddings[i * DIMENSION_EMBEDDING + j] = E[token_id * DIMENSION_EMBEDDING + j];
        }
    }
} 

void init_rand(float *X, int size) { 
    for (int i = 0; i < size; i++) {
        X[i] = ((float)rand() / RAND_MAX) * 0.1f;  // Small random values
    }
} 
     
    

int main() { 
    // TODO: load the byte pair encoding from the previous program.
    // TODO: accept input from the user in the command line and then run the forward part
    
    // Maximum sequence length for which we pre-compute positional encodings
    int max_seq_len = 10000;
    float* pe = malloc(max_seq_len * DIMENSION_EMBEDDING * sizeof(float));
    compute_positional_encoding(pe, max_seq_len, DIMENSION_EMBEDDING);

    
    // dummy 100 token input
    int size = 10;
    float input[size];
    init_rand(&input[0], size);
    
    // Assuming vocabulary size (for one-hot encoding)
    // learned embedding projection
    float* E = malloc(VOCAB_SIZE * DIMENSION_EMBEDDING * sizeof(float));
    // embedding vector that will be input into model
    float* embeddings = malloc(size * DIMENSION_EMBEDDING * sizeof(float));
    convert_to_embeddings(E, embeddings, &input[0], size);
    
    // Add positional encodings to the embeddings
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < DIMENSION_EMBEDDING; j++) {
            // Add the positional encoding to the token embedding
            // This ensures the model can distinguish between tokens at different positions
            embeddings[i * DIMENSION_EMBEDDING + j] += pe[i * DIMENSION_EMBEDDING + j];
        }
    }


    // dummy scaled attention, 
    float *Q = malloc(size* DIMENSION_KEYS * NUM_HEADS * sizeof(float));
    float *K = malloc(size * DIMENSION_KEYS * NUM_HEADS * sizeof(float));
    float *V = malloc(size * DIMENSION_VALUES * NUM_HEADS * sizeof(float));

    float* W_Q = malloc(NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_KEYS * sizeof(float));
    float* W_K = malloc(NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_KEYS * sizeof(float));
    float* W_V = malloc(NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_VALUES * sizeof(float));
    float* W_O = malloc(DIMENSION_HIDDEN * DIMENSION_HIDDEN * sizeof(float));

    init_rand(W_Q, NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_KEYS);
    init_rand(W_K, NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_KEYS);
    init_rand(W_V, NUM_HEADS * DIMENSION_HIDDEN * DIMENSION_VALUES);
    init_rand(W_O, DIMENSION_HIDDEN * DIMENSION_HIDDEN);

    float *result = malloc(NUM_HEADS * size * DIMENSION_VALUES * sizeof(float));
    // Initialize result array to zeros

    // Apply attention for each head
    for (int head = 0; head < NUM_HEADS; head++) {
        float *Q_head = Q + (head * size * DIMENSION_KEYS);
        float *K_head = K + (head * size * DIMENSION_KEYS);
        float *V_head = V + (head * size * DIMENSION_VALUES);
        float *result_head = result + (head * size * DIMENSION_VALUES);
        
        // Project embeddings to Q, K, V for this head using weight matrices
        float *W_Q_head = W_Q + (head * DIMENSION_HIDDEN * DIMENSION_KEYS);
        float *W_K_head = W_K + (head * DIMENSION_HIDDEN * DIMENSION_KEYS);
        float *W_V_head = W_V + (head * DIMENSION_HIDDEN * DIMENSION_VALUES);
        
        // Project embeddings to Q, K, V matrices
        matmul(embeddings, W_Q_head, size, DIMENSION_HIDDEN, DIMENSION_KEYS, Q_head);
        matmul(embeddings, W_K_head, size, DIMENSION_HIDDEN, DIMENSION_KEYS, K_head);
        matmul(embeddings, W_V_head, size, DIMENSION_HIDDEN, DIMENSION_VALUES, V_head);
        
        attention(Q_head, K_head, V_head, size, result_head);
    }



   // printf("elements of result\n");
   // for(int i = 0; i < 10; i ++) { 
   //     printf("%f\n", result[NUM_HEADS * DIMENSION_VALUES * size + i]);
   //     printf("%f\n", result[i]);
   // } 

    fprintf(stdout, "attention finished\n");

    free(pe);
    free(E);
    free(embeddings);
    free(W_Q);
    free(W_K);
    free(W_V);
    free(W_O);
    free(Q);
    free(K);
    free(V);
    free(result);

} 
