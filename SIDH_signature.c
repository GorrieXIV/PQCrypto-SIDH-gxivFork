/*************************************************************************
* A Post-Quantum Digital Signature Scheme Based on Supersingular Isogenies
*
* Copyright (c) Youngho Yoo.
*
* Abstract: Testing the isogeny-based signature scheme.
*
* Ported to Microsoft's SIDH 2.0 Library by Robert Gorrie (gxiv)
*************************************************************************/

#include "SIDH_signature.h"
#include "tests/test_extras.h"
#include "keccak.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>

int NUM_THREADS = 248;
int CUR_ROUND = 0;
int batchSize = 248;
int errorCount = 0;
int roundSuccess = 0;
int psiS_count = 0;
batch_struct* signBatchA;
batch_struct* signBatchB;
batch_struct* verifyBatchA;
batch_struct* verifyBatchB;
batch_struct* verifyBatchC;
batch_struct* compressionBatch;
batch_struct* decompressionBatch;
pthread_mutex_t RLOCK;      //lock for round counter
pthread_mutex_t BLOCK;      //lock for batch size counter
pthread_mutex_t ELOCK;      //lock for errorCount

digit_t a[NWORDS_ORDER], b[NWORDS_ORDER];

void hashdata(unsigned int pbytes, unsigned char** comm1, unsigned char** comm2, uint8_t* HashResp, int hlen, int dlen, uint8_t *data, uint8_t *cHash, int cHashLength) {
    int r;
    for (r=0; r<NUM_ROUNDS; r++) {
        memcpy(data + (r * 2*pbytes), comm1[r], 2*pbytes);
        memcpy(data + (NUM_ROUNDS * 2*pbytes) + (r * 2*pbytes), comm2[r], 2*pbytes);
    }
    memcpy(data + (2 * NUM_ROUNDS * 2*pbytes), HashResp, 2 * NUM_ROUNDS * hlen);

    keccak(data, dlen, cHash, cHashLength);
}

CRYPTO_STATUS isogeny_keygen(PCurveIsogenyStruct CurveIsogeny, unsigned char *PrivateKey, unsigned char *PublicKey) {
    unsigned int pbytes = (CurveIsogeny->pwordbits + 7)/8;      // Number of bytes in a field element
    unsigned int n, obytes = (CurveIsogeny->owordbits + 7)/8;   // Number of bytes in an element in [1, order]
    bool valid_PublicKey = false;
    CRYPTO_STATUS Status = CRYPTO_SUCCESS;
    bool passed;

    // Generate Peggy(Bob)'s keys
    passed = true;
    Status = KeyGeneration_B(PrivateKey, PublicKey, CurveIsogeny);
    if (Status != CRYPTO_SUCCESS) {
        passed = false;
    }

    if (!passed) {
      #ifdef TEST_RUN_PRINTS
    	printf("  Key generation failed\n"); goto cleanup;
      #endif
    }

cleanup:

    return Status;
}

typedef struct thread_params_sign {
	PCurveIsogenyStruct *CurveIsogeny;
	unsigned char *PrivateKey;
	unsigned char *PublicKey;
	struct Signature *sig;

	unsigned int pbytes;
	unsigned int n;
	unsigned int obytes;

	int compressed;
} thread_params_sign;


void *sign_thread(void *TPS) {
	CRYPTO_STATUS Status = CRYPTO_SUCCESS;
	thread_params_sign *tps = (thread_params_sign*) TPS;

	int r=0;

	while (1) {
		int stop=0;

		pthread_mutex_lock(&RLOCK);
		if (CUR_ROUND >= NUM_ROUNDS) {
			stop=1;
		} else {
			r = CUR_ROUND;
			CUR_ROUND++;
		}
		pthread_mutex_unlock(&RLOCK);

		if (stop) break;

		tps->sig->Randoms[r] = (unsigned char*)calloc(1, tps->obytes);
		tps->sig->Commitments1[r] = (unsigned char*)calloc(1, 2*tps->pbytes);
		tps->sig->Commitments2[r] = (unsigned char*)calloc(1, 2*tps->pbytes);
		tps->sig->psiS[r] = calloc(1, sizeof(point_proj));
		tps->sig->compressed = tps->compressed;

		// Pick random point R and compute E/<R>
		f2elm_t A;

		unsigned char *TempPubKey;
		TempPubKey = (unsigned char*)calloc(1, 4*2*tps->pbytes);

		Status = KeyGeneration_A(tps->sig->Randoms[r], TempPubKey, *(tps->CurveIsogeny), true, signBatchA);
		//check success of KeyGeneration_A
		if(Status != CRYPTO_SUCCESS) {
      #ifdef TEST_RUN_PRINTS
			printf("Random point generation failed\n");
      #endif
		}

		to_fp2mont(((f2elm_t*)TempPubKey)[0], A);
		fp2copy751(A, *(f2elm_t*)tps->sig->Commitments1[r]);     //commitment1[r] = A = tempPubKey[0]
    /*
    printf("Sign A[%d]:   ", r);
    for (int i = 0; i < 2*tps->pbytes; i++) {
      printf("%0hhu", (tps->sig->Commitments1[r])[i]);
    } printf("\n");
    */
		point_proj tempPsiS[1];

		//although SecretAgreement_A runs faster than B, B appears necessary so that we can generate psiS
		Status = SecretAgreement_B(tps->PrivateKey, TempPubKey, tps->sig->Commitments2[r], *(tps->CurveIsogeny), NULL, tempPsiS, signBatchB);
    if(Status != CRYPTO_SUCCESS) {
      #ifdef TEST_RUN_PRINTS
			printf("Secret Agreement failed\n");
      #endif
		}

		f2elm_t Ab;
		//to_fp2mont(((f2elm_t*)tps->PublicKey)[0],Ab);
    to_fp2mont(((f2elm_t*)TempPubKey)[0], Ab);



		if (tps->compressed) {
			Status = compressPsiS(tempPsiS, tps->sig->compPsiS[r], &(tps->sig->compBit[r]), tps->sig->Commitments1[r], *(tps->CurveIsogeny), compressionBatch);
      //Status = compressPsiS_test(tempPsiS, tps->sig->compPsiS[r], &(tps->sig->compBit[r]), tps->sig->Commitments1[r], *(tps->CurveIsogeny), NULL, a, b);
      #ifdef COMPARE_COMPRESSED_PSIS_PRINTS
        printf("Sign round %d: ", r);
        printf_digit_order("comp", tps->sig->compPsiS[r], NWORDS_ORDER);
      #endif
      if (Status != CRYPTO_SUCCESS) {
				if (Status == CRYPTO_ERROR_DURING_TEST) {
          #ifdef TEST_RUN_PRINTS
					printf("half_ph3 not working\n");
          #endif
				} else {
          #ifdef TEST_RUN_PRINTS
					printf("Error in psi(S) compression on round %d\n", r);
          #endif
				}
				pthread_mutex_lock(&ELOCK);
				errorCount++;
				pthread_mutex_unlock(&ELOCK);
			}
		} else {
			fp2copy751(tempPsiS->X, tps->sig->psiS[r]->X);
			fp2copy751(tempPsiS->Z, tps->sig->psiS[r]->Z);
		}

		//check success of SecretAgreementB

	}

}


CRYPTO_STATUS isogeny_sign(PCurveIsogenyStruct CurveIsogeny, unsigned char *PrivateKey, unsigned char *PublicKey, struct Signature *sig, int batched, int compressed) {
	unsigned int pbytes = (CurveIsogeny->pwordbits + 7)/8;          // Number of bytes in a field element
	unsigned int pwords = NBITS_TO_NWORDS(CurveIsogeny->pwordbits); // Number of words in a curve element
	unsigned int n, obytes = (CurveIsogeny->owordbits + 7)/8;       // Number of bytes in an element in [1, order]
	unsigned long long cycles, cycles1, cycles2, totcycles=0;

	CRYPTO_STATUS Status = CRYPTO_SUCCESS;
	bool passed;

	// Run the ZKP rounds
	int r;
	pthread_t sign_threads[NUM_THREADS];

	CUR_ROUND = 0;
	if (pthread_mutex_init(&RLOCK, NULL)) {
    #ifdef TEST_RUN_PRINTS
		printf("ERROR: mutex init failed\n");
    #endif
		return 1;
	}

	errorCount = 0;
	if (pthread_mutex_init(&ELOCK, NULL)) {
    #ifdef TEST_RUN_PRINTS
		printf("ERROR: error counter mutex init failed\n");
    #endif
		return 1;
	}

	thread_params_sign tps = {&CurveIsogeny, PrivateKey, PublicKey, sig, pbytes, n, obytes, compressed};

	if (batched) {
		signBatchA = (batch_struct*) malloc (sizeof(batch_struct));
		signBatchA->batchSize = 248;
		signBatchA->cntr = 0;
		signBatchA->invArray = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
		signBatchA->invDest = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
		pthread_mutex_init(&signBatchA->arrayLock, NULL);
		sem_init(&signBatchA->sign_sem, 0, 0);

		signBatchB = (batch_struct*) malloc (sizeof(batch_struct));
		signBatchB->batchSize = 248;
		signBatchB->cntr = 0;
		signBatchB->invArray = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
		signBatchB->invDest = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
		pthread_mutex_init(&signBatchB->arrayLock, NULL);
		sem_init(&signBatchB->sign_sem, 0, 0);

    if (compressed) {
      compressionBatch = (batch_struct*) malloc (sizeof(batch_struct));
      compressionBatch->batchSize = 248;
      compressionBatch->cntr = 0;
      compressionBatch->invArray = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
      compressionBatch->invDest = (f2elm_t*) malloc (248 * sizeof(f2elm_t));
      pthread_mutex_init(&compressionBatch->arrayLock, NULL);
      sem_init(&compressionBatch->sign_sem, 0, 0);
    } else {
      compressionBatch = NULL;
    }
	} else {
		signBatchA = NULL;
		signBatchB = NULL;
    compressionBatch = NULL;
	}


	int t;
	for (t=0; t<NUM_THREADS; t++) {
		if (pthread_create(&sign_threads[t], NULL, sign_thread, &tps)) {
      #ifdef TEST_RUN_PRINTS
			printf("ERROR: Failed to create thread %d\n", t);
      #endif
		}
	}

	for (t=0; t<NUM_THREADS; t++) {
		pthread_join(sign_threads[t], NULL);
	}

	if (errorCount > 0) {
		//return CRYPTO_ERROR_INVALID_ORDER;
	}

	//printf("Average time for ZKP round ...... %10lld cycles\n", totcycles/NUM_ROUNDS);

	// Commit to responses (hash)
	int HashLength = 32; //bytes
	sig->HashResp = calloc(2*NUM_ROUNDS, HashLength*sizeof(uint8_t));

	for (r=0; r<NUM_ROUNDS; r++) {
		keccak((uint8_t*) sig->Randoms[r], obytes, sig->HashResp+((2*r)*HashLength), HashLength);
		if (sig->compressed) {
			keccak((uint8_t*) sig->compPsiS[r], sizeof(digit_t) * NWORDS_ORDER, sig->HashResp+((2*r+1)*HashLength), HashLength);
		} else {
			keccak((uint8_t*) sig->psiS[r], sizeof(point_proj), sig->HashResp+((2*r+1)*HashLength), HashLength);
		}
	}

	// Create challenge hash (by hashing all the commitments and HashResps)
	uint8_t *datastring, *cHash;
	int DataLength = (2 * NUM_ROUNDS * 2*pbytes) + (2 * NUM_ROUNDS * HashLength*sizeof(uint8_t));
	int cHashLength = NUM_ROUNDS/8;
	datastring = calloc(1, DataLength);
	cHash = calloc(1, cHashLength);

	hashdata(pbytes, sig->Commitments1, sig->Commitments2, sig->HashResp, HashLength, DataLength, datastring, cHash, cHashLength);

	pthread_t compress_threads[NUM_THREADS/3];


cleanup:
		if (batched) {
			free(signBatchA->invArray);
			free(signBatchA->invDest);
			free(signBatchB->invArray);
			free(signBatchB->invDest);
      if (compressed) {
        free(compressionBatch->invArray);
        free(compressionBatch->invDest);
      }
		}


	return Status;
}



typedef struct thread_params_verify {
	PCurveIsogenyStruct *CurveIsogeny;
	unsigned char *PublicKey;
	struct Signature *sig;

	int cHashLength;
	uint8_t *cHash;

	unsigned int pbytes;
	unsigned int n;
	unsigned int obytes;

	int compressed;
} thread_params_verify;

void *verify_thread(void *TPV) {
	CRYPTO_STATUS Status = CRYPTO_SUCCESS;
	thread_params_verify *tpv = (thread_params_verify*) TPV;

	// iterate through cHash bits as challenge and verify
	bool verified = true;
	int r=0;
	int i,j;

	while (1) {
		int stop=0;
		verified = true;

		pthread_mutex_lock(&RLOCK);
		if (CUR_ROUND >= NUM_ROUNDS) {
			stop=1;
		} else {
			r = CUR_ROUND;
			CUR_ROUND++;
		}
		pthread_mutex_unlock(&RLOCK);

		if (stop) break;

		//printf("\nround: %d ", CUR_ROUND);
		i = r/8;
		j = r%8;

		int bit = tpv->cHash[i] & (1 << j);  //challenge bit

		if (bit == 0) {
			pthread_mutex_lock(&BLOCK);
			if (verifyBatchA != NULL && verifyBatchB != NULL) {
				verifyBatchA->batchSize++;
				verifyBatchB->batchSize++;
			}
			pthread_mutex_unlock(&BLOCK);
			//printf("round %d: bit 0 - ", r);

			// Check R, phi(R) has order 2^372 (suffices to check that the random number is even)
			uint8_t lastbyte = ((uint8_t*) tpv->sig->Randoms[r])[0];
			if (lastbyte % 2) {
        #ifdef TEST_RUN_PRINTS
				printf("ERROR: R, phi(R) are not full order\n");
        #endif
			} else {
				//printf("checked order. ");
			}

			// Check kernels
			f2elm_t A;
			unsigned char *TempPubKey;
			TempPubKey = (unsigned char*)calloc(1, 4*2*tpv->pbytes);

			Status = KeyGeneration_A(tpv->sig->Randoms[r], TempPubKey, *(tpv->CurveIsogeny), false, verifyBatchA);

			if(Status != CRYPTO_SUCCESS) {
        #ifdef TEST_RUN_PRINTS
				printf("Computing E -> E/<R> failed");
        #endif
			} else {
				//printf("%s %d: thread success of KeyGenA\n", __FILE__, __LINE__);
			}


			to_fp2mont(((f2elm_t*)TempPubKey)[0], A);

			int cmp = memcmp(A, tpv->sig->Commitments1[r], sizeof(f2elm_t));
			if (cmp != 0) {
				verified = false;
        #ifdef TEST_RUN_PRINTS
				printf("verifying E -> E/<R> failed\n");
        #endif
			}

			unsigned char *TempSharSec;
			TempSharSec = (unsigned char*)calloc(1, 2*tpv->pbytes);

			Status = SecretAgreement_A(tpv->sig->Randoms[r], tpv->PublicKey, TempSharSec, *(tpv->CurveIsogeny), NULL, verifyBatchB);
			if(Status != CRYPTO_SUCCESS) {
        #ifdef TEST_RUN_PRINTS
				printf("Computing E/<S> -> E/<R,S> failed");
        #endif
			} else {
				//printf("%s %d: thread success of SecAgrA\n", __FILE__, __LINE__);
			}

			cmp = memcmp(TempSharSec, tpv->sig->Commitments2[r], 2*tpv->pbytes);
			if (cmp != 0) {
				verified = false;
        #ifdef TEST_RUN_PRINTS
				printf("verifying E/<S> -> E/<R,S> failed on non-compressed path\n");
        #endif
			}

		} else {
			pthread_mutex_lock(&BLOCK);
			if (verifyBatchC != NULL) {
				verifyBatchC->batchSize++;
        if (decompressionBatch != NULL) {
          decompressionBatch->batchSize++;
        }
			}
			pthread_mutex_unlock(&BLOCK);

			// Check psi(S) has order 3^239 (need to triple it 239 times)
			point_proj_t triple = {0};
			point_proj_t newPsiS = {0};
			f2elm_t A,C={0};
			fp2copy751(tpv->sig->Commitments1[r], A);

			if (tpv->compressed) {
        #ifdef COMPARE_COMPRESSED_PSIS_PRINTS
          printf("Verify round %d: ", r);
          printf_digit_order("comp", tpv->sig->compPsiS[r], NWORDS_ORDER);
        #endif
				Status = decompressPsiS(tpv->sig->compPsiS[r], triple, tpv->sig->compBit[r], A, *(tpv->CurveIsogeny), decompressionBatch);
        //Status = decompressPsiS_test(tpv->sig->compPsiS[r], triple, tpv->sig->compBit[r], A, *(tpv->CurveIsogeny), a, b);

        if (Status != CRYPTO_SUCCESS) {
          #ifdef TEST_RUN_PRINTS
					printf("Error in psi(S) decompression\n");
          #endif
					errorCount++;
				} else {
					copy_words((digit_t*)triple, (digit_t*)newPsiS, 2*2*NWORDS_FIELD);
				}
			} else {
				copy_words((digit_t*)tpv->sig->psiS[r], (digit_t*)triple, 2*2*NWORDS_FIELD);
				copy_words((digit_t*)tpv->sig->psiS[r], (digit_t*)newPsiS, 2*2*NWORDS_FIELD);
			}

			to_fp2mont(((f2elm_t*)tpv->PublicKey)[0],A);
			fpcopy751((*(tpv->CurveIsogeny))->C, C[0]);
			int t;
			for (t=0; t<238; t++) {
				xTPL(triple, triple, A, C); //triple psiS to check if order(psiS) = 3^239
				if (is_felm_zero(((felm_t*)triple->Z)[0]) && is_felm_zero(((felm_t*)triple->Z)[1])) {
          #ifdef TEST_RUN_PRINTS
          printf("ERROR: psi(S) has order 3^%d\n", t+1);
          #endif
          break;
				}
			}

			unsigned char *TempSharSec, *TempPubKey;
			TempSharSec = calloc(1, 2*tpv->pbytes);
			TempPubKey = calloc(1, 4*2*tpv->pbytes);
			from_fp2mont(tpv->sig->Commitments1[r], ((f2elm_t*)TempPubKey)[0]);

			//if this secret agreement is successful, we know psiS has order la^ea and generates the kernel of E1 -> E2
			//can we do this in a method simpler and quicker using only a & b where psiS = [a]R1 + [b]R2
			Status = SecretAgreement_B(NULL, TempPubKey, TempSharSec, *(tpv->CurveIsogeny), newPsiS, NULL, verifyBatchC);
			if(Status != CRYPTO_SUCCESS) {
        #ifdef TEST_RUN_PRINTS
				printf("Computing E/<R> -> E/<R,S> failed");
        #endif
			}

      //only look at x in affine otherwise false negatives
			int cmp = memcmp(TempSharSec, tpv->sig->Commitments2[r], 2*tpv->pbytes);
			if (cmp != 0) {
				verified = false;
        #ifdef TEST_RUN_PRINTS
				printf("verifying E/<R> -> E/<R,S> failed on compressed path\n");
        #endif
			}

			if (tpv->sig->compressed) {
				if (!verified) {
					pthread_mutex_lock(&ELOCK);
					errorCount++;
					pthread_mutex_unlock(&ELOCK);
          #ifdef COMPRESSION_TEST_PRINTS
					printf("Error in verify on round %d\n", r);
          #endif
				}
			}

		}

	}

}


CRYPTO_STATUS isogeny_verify(PCurveIsogenyStruct CurveIsogeny, unsigned char *PublicKey, struct Signature *sig, int batched, int compressed) {
	unsigned int pbytes = (CurveIsogeny->pwordbits + 7)/8;      // Number of bytes in a field element
	unsigned int n, obytes = (CurveIsogeny->owordbits + 7)/8;   // Number of bytes in an element in [1, order]
	unsigned long long cycles, cycles1, cycles2, totcycles=0;
	CRYPTO_STATUS Status = CRYPTO_SUCCESS;
	bool passed;

	int r;

	// compute challenge hash
	int HashLength = 32;
	int cHashLength = NUM_ROUNDS/8;
	int DataLength = (2 * NUM_ROUNDS * 2*pbytes) + (2 * NUM_ROUNDS * HashLength*sizeof(uint8_t));
	uint8_t *datastring, *cHash;
	datastring = calloc(1, DataLength);
	cHash = calloc(1, cHashLength);

	hashdata(pbytes, sig->Commitments1, sig->Commitments2, sig->HashResp, HashLength, DataLength, datastring, cHash, cHashLength);

	// Run the verifying rounds
	pthread_t verify_threads[NUM_THREADS];

	//initialize mutexes and cross-thread variables
	CUR_ROUND = 0;
	if (pthread_mutex_init(&RLOCK, NULL)) {
    #ifdef TEST_RUN_PRINTS
		printf("ERROR: mutex init failed\n");
    #endif
		return 1;
	}

	errorCount = 0;
	if (pthread_mutex_init(&ELOCK, NULL)) {
    #ifdef TEST_RUN_PRINTS
		printf("ERROR: mutex init failed\n");
    #endif
		return 1;
	}

	thread_params_verify tpv = {&CurveIsogeny, PublicKey, sig, cHashLength, cHash, pbytes, n, obytes, compressed};

  int bit;
  for (int iter=0; iter<NUM_THREADS; iter++) {
    int hash_index = iter/8;
    int word_shift = iter%8;
    bit = tpv.cHash[hash_index] & (1 << word_shift);
    if (bit != 0) {
      psiS_count++;
    }
  }

	if (batched) {
		verifyBatchA = (batch_struct*) malloc (sizeof(batch_struct));
		verifyBatchA->batchSize = 0;//248 - psiS_count;
		verifyBatchA->cntr = 0;
		verifyBatchA->invArray = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		verifyBatchA->invDest = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		pthread_mutex_init(&verifyBatchA->arrayLock, NULL);
		sem_init(&verifyBatchA->sign_sem, 0, 0);

		verifyBatchB = (batch_struct*) malloc (sizeof(batch_struct));
		verifyBatchB->batchSize = 0;//248 - psiS_count;
		verifyBatchB->cntr = 0;
		verifyBatchB->invArray = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		verifyBatchB->invDest = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		pthread_mutex_init(&verifyBatchB->arrayLock, NULL);
		sem_init(&verifyBatchB->sign_sem, 0, 0);

		verifyBatchC = (batch_struct*) malloc (sizeof(batch_struct));
		verifyBatchC->batchSize = 0;//psiS_count;
		verifyBatchC->cntr = 0;
		verifyBatchC->invArray = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		verifyBatchC->invDest = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
		pthread_mutex_init(&verifyBatchC->arrayLock, NULL);
		sem_init(&verifyBatchC->sign_sem, 0, 0);

    if (compressed) {
      decompressionBatch = (batch_struct*) malloc (sizeof(batch_struct));
      decompressionBatch->batchSize = 0;//psiS_count;
      decompressionBatch->cntr = 0;
      decompressionBatch->invArray = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
      decompressionBatch->invDest = (f2elm_t*) malloc (batchSize * sizeof(f2elm_t));
      pthread_mutex_init(&decompressionBatch->arrayLock, NULL);
      sem_init(&decompressionBatch->sign_sem, 0, 0);
    } else {
      decompressionBatch = NULL;
    }
	} else {
		verifyBatchA = NULL;
		verifyBatchB = NULL;
		verifyBatchC = NULL;
    decompressionBatch = NULL;
	}

	int t;
	for (t=0; t<NUM_THREADS; t++) {
		if (pthread_create(&verify_threads[t], NULL, verify_thread, &tpv)) {
      #ifdef TEST_RUN_PRINTS
			printf("ERROR: Failed to create thread %d\n", t);
      #endif
		}
	}

	for (t=0; t<NUM_THREADS; t++) {
		pthread_join(verify_threads[t], NULL);
	}

	if (errorCount > 0) {
		return CRYPTO_ERROR_INVALID_ORDER;
	}

cleanup:
		if (batched) {
			free(verifyBatchA->invArray);
			free(verifyBatchA->invDest);
			free(verifyBatchB->invArray);
			free(verifyBatchB->invDest);
			free(verifyBatchC->invArray);
			free(verifyBatchC->invDest);
      if (compressed) {
        free(decompressionBatch->invArray);
        free(decompressionBatch->invDest);
      }
		}

    return Status;
}
