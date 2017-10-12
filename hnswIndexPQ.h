#pragma once

#include <fstream>
#include <cstdio>
#include <vector>
#include <queue>
#include <limits>


#include "L2space.h"
#include "brutoforce.h"
#include "hnswalg.h"
#include <faiss/ProductQuantizer.h>
#include <faiss/utils.h>
#include <faiss/index_io.h>

#define WRITEANDCHECK(ptr, n, f) {                                 \
        size_t ret = fwrite (ptr, sizeof (* (ptr)), n, f);         \
    }

#define READANDCHECK(ptr, n, f) {                                  \
        size_t ret = fread (ptr, sizeof (* (ptr)), n, f);          \
    }

#define WRITE1(x, f) WRITEANDCHECK(&(x), 1, f)
#define READ1(x, f)  READANDCHECK(&(x), 1, f)

#define WRITEVECTOR(vec, f) {                     \
        size_t size = (vec).size();            \
        WRITEANDCHECK (&size, 1, f);              \
        WRITEANDCHECK ((vec).data(), size, f);    \
    }

#define READVECTOR(vec, f) {                       \
        long size;                            \
        READANDCHECK (&size, 1, f);                \
        (vec).resize (size);                    \
        READANDCHECK ((vec).data(), size, f);     \
    }


typedef unsigned int idx_t;
typedef unsigned char uint8_t;

inline bool exists_test(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}




template <typename format>
static void readXvec(std::ifstream &input, format *mass, const int d, const int n = 1)
{
	int in = 0;
    for (int i = 0; i < n; i++) {
        input.read((char *) &in, sizeof(int));
        if (in != d) {
            std::cout << "file error\n";
            exit(1);
        }
        input.read((char *)(mass+i*d), in * sizeof(format));
    }
}

namespace hnswlib {

    void read_pq(const char *path, faiss::ProductQuantizer *_pq)
    {
        if (!_pq) {
            std::cout << "PQ object does not exists" << std::endl;
            return;
        }
        FILE *fin = fopen(path, "rb");

        fread(&_pq->d, sizeof(size_t), 1, fin);
        fread(&_pq->M, sizeof(size_t), 1, fin);
        fread(&_pq->nbits, sizeof(size_t), 1, fin);
        _pq->set_derived_values ();

        size_t size;
        fread (&size, sizeof(size_t), 1, fin);
        _pq->centroids.resize(size);

        float *centroids = _pq->centroids.data();
        fread(_pq->centroids.data(), sizeof(float), size, fin);

        fclose(fin);
    }

    void write_pq(const char *path, faiss::ProductQuantizer *_pq)
    {
        if (!_pq){
            std::cout << "PQ object does not exist" << std::endl;
            return;
        }
        FILE *fout = fopen(path, "wb");

        fwrite(&_pq->d, sizeof(size_t), 1, fout);
        fwrite(&_pq->M, sizeof(size_t), 1, fout);
        fwrite(&_pq->nbits, sizeof(size_t), 1, fout);

        size_t size = _pq->centroids.size();
        fwrite (&size, sizeof(size_t), 1, fout);

        float *centroids = _pq->centroids.data();
        fwrite(_pq->centroids.data(), sizeof(float), size, fout);

        fclose(fout);
    }


    struct Index
	{
		size_t d;
		size_t csize;
        size_t code_size;

        /** Query members **/
        size_t nprobe = 16;
        size_t max_codes = 10000;

		faiss::ProductQuantizer *norm_pq;
        faiss::ProductQuantizer *pq;

		std::vector < std::vector<uint8_t> > codes;
        std::vector < std::vector<idx_t> > ids;

        float *c_norm_table;
		HierarchicalNSW<float, float> *quantizer;


    public:
		Index(size_t dim, size_t ncentroids,
			  size_t bytes_per_code, size_t nbits_per_idx):
				d(dim), csize(ncentroids)
		{
            codes.reserve(ncentroids);
            ids.reserve(ncentroids);

            pq = new faiss::ProductQuantizer(dim, bytes_per_code, nbits_per_idx);
            norm_pq = new faiss::ProductQuantizer(1, 1, nbits_per_idx);
        }


		~Index() {
            if (c_norm_table)
                delete c_norm_table;

            delete pq;
            delete norm_pq;
            delete quantizer;
		}

		void buildQuantizer(SpaceInterface<float> *l2space, const char *path_clusters,
                            const char *path_info, const char *path_edges, int efSearch)
		{
            if (exists_test(path_info) && exists_test(path_edges)) {
                quantizer = new HierarchicalNSW<float, float>(l2space, path_info, path_clusters, path_edges);
                quantizer->ef_ = efSearch;
                return;
            }
            quantizer = new HierarchicalNSW<float, float>(l2space, {{csize, {16, 32}}}, 240);
            quantizer->ef_ = efSearch;

			std::cout << "Constructing quantizer\n";
			int j1 = 0;
			std::ifstream input(path_clusters, ios::binary);

			float mass[d];
			readXvec<float>(input, mass, d);
			quantizer->addPoint((void *) (mass), j1);

			size_t report_every = 100000;
		#pragma omp parallel for num_threads(16)
			for (int i = 1; i < csize; i++) {
				float mass[d];
		#pragma omp critical
				{
					readXvec<float>(input, mass, d);
					if (++j1 % report_every == 0)
						std::cout << j1 / (0.01 * csize) << " %\n";
				}
				quantizer->addPoint((void *) (mass), (size_t) j1);
			}
			input.close();
			quantizer->SaveInfo(path_info);
			quantizer->SaveEdges(path_edges);
		}


		void assign(size_t n, float *data, idx_t *precomputed_idx)
		{
		    //#pragma omp parallel for num_threads(16)
			for (int i = 0; i < n; i++)
				precomputed_idx[i] = quantizer->searchKnn((data + i*d), 1).top().second;
		}


		void add(idx_t n, float * x, const idx_t *xids, const idx_t *precomputed_idx)
		{
			const idx_t * idx = precomputed_idx;

			uint8_t * xcodes = new uint8_t [n * code_size];
			uint8_t *norm_codes = new uint8_t[n];

			float *to_encode = nullptr;
			float *norm_to_encode = new float[n];

            float *residuals = new float [n * d];
            for (size_t i = 0; i < n; i++)
                compute_residual(x + i * d, residuals + i * d, idx[i]);
            to_encode = residuals;

			pq->compute_codes (to_encode, xcodes, n);

            float *decoded_x = new float[n*d];
            pq->decode(xcodes, decoded_x, n);
            for (idx_t i = 0; i < n; i++) {
                float *centroid = (float *) quantizer->getDataByInternalId(precomputed_idx[i]);
                for (int j = 0; j < d; j++)
                    decoded_x[i*d + j] += centroid[j];

            }
            faiss::fvec_norms_L2sqr (norm_to_encode, decoded_x, d, n);
			norm_pq->compute_codes(norm_to_encode, norm_codes, n);

			for (size_t i = 0; i < n; i++) {
				idx_t key = idx[i];
				idx_t id = xids[i];
				ids[key].push_back(id);
				uint8_t *code = xcodes + i * code_size;
				for (size_t j = 0; j < code_size; j++)
					codes[key].push_back (code[j]);
				codes[key].push_back(norm_codes[i]);
			}

            delete to_encode;
            delete decoded_x;
			delete xcodes;
			delete norm_to_encode;
			delete norm_codes;
		}

		void search (float *x, idx_t k, idx_t *results)
		{
            idx_t keys[nprobe];
            float q_c[nprobe];

            float * dis_tables = new float [pq->ksub * pq->M];
            pq->compute_inner_prod_table(x, dis_tables);

            std::priority_queue<std::pair<float, idx_t>> topResults;
            auto coarse = quantizer->searchKnn(x, nprobe);

            for (int i = nprobe - 1; i >= 0; i--) {
                auto elem = coarse.top();
                q_c[i] = elem.first;
                keys[i] = elem.second;
                coarse.pop();
            }
            for (int i = 0; i < nprobe; i++){
                idx_t key = keys[i];
                std::vector<uint8_t> code = codes[key];
                float term1 = q_c[i] - c_norm_table[key];
                int ncodes = code.size()/(code_size+1);

                //if (i < 1)
                //    std::cout << q_c[i] << " " << c << std::endl;
                for (int j = 0; j < ncodes; j++){
                    float q_r = 0.;
                    for (int m = 0; m < code_size; m++)
                        q_r += dis_tables[pq->ksub * m + code[j*(code_size + 1) + m]];

                    float norm;
                    norm_pq->decode(code.data()+j*(code_size+1) + code_size, &norm);
                    float dist = term1 - 2*q_r + norm;

                    // << " " << q_r << " " << norm << std::endl;
                    idx_t label = ids[key][j];
                    topResults.emplace(std::make_pair(dist, label));
                }
                if (topResults.size() > max_codes)
                    break;
            }

            while (topResults.size() > k)
                topResults.pop();

            if (topResults.size() < k) {
                for (int j = topResults.size(); j < k; j++)
                    topResults.emplace(std::make_pair(std::numeric_limits<float>::max(), 0));
                std::cout << "Ignored query" << std:: endl;
            }
            for (int j = k-1; j >= 0; j--) {
                results[j] = topResults.top().second;
                topResults.pop();
            }
		}

        void compute_centroid_norm_table()
        {
            c_norm_table = new float[csize];
            for (int i = 0; i < csize; i++){
                float *c = (float *)quantizer->getDataByInternalId(i);
                faiss::fvec_norms_L2sqr (c_norm_table+i, c, d, 1);
            }
        }

        void train_norm_pq(idx_t n, float *x)
        {
            idx_t *assigned = new idx_t [n]; // assignement to coarse centroids
            assign (n, x, assigned);
            float *residuals = new float [n * d];
            for (idx_t i = 0; i < n; i++)
                compute_residual (x + i * d, residuals+i*d, assigned[i]);

            uint8_t * xcodes = new uint8_t [n * code_size];
            pq->compute_codes (residuals, xcodes, n);
            pq->decode(xcodes, residuals, n);

            float *decoded_x = new float[n*d];
            for (idx_t i = 0; i < n; i++) {
                float *centroid = (float *) quantizer->getDataByInternalId(assigned[i]);
                for (int j = 0; j < d; j++)
                    decoded_x[i*d + j] = centroid[j] + residuals[i*d + j];

            }
            delete residuals;
            delete assigned;
            delete xcodes;

            float *trainset = new float[n];
            faiss::fvec_norms_L2sqr (trainset, decoded_x, d, n);

            norm_pq->verbose = true;
            norm_pq->train (n, trainset);

            delete trainset;
        }

        void train_residual_pq(idx_t n, float *x)
        {
            const float *trainset;
            float *residuals;
            idx_t * assigned;

            printf("Computing residuals\n");
            assigned = new idx_t [n]; // assignement to coarse centroids
            assign (n, x, assigned);

            residuals = new float [n * d];
            for (idx_t i = 0; i < n; i++)
                compute_residual (x + i * d, residuals+i*d, assigned[i]);

            trainset = residuals;

            printf ("Training %zdx%zd product quantizer on %ld vectors in %dD\n",
                    pq->M, pq->ksub, n, d);
            pq->verbose = true;
            pq->train (n, trainset);

            delete assigned;
            delete residuals;
        }


        void precompute_idx(size_t n, const char *path_data, const char *fo_name)
        {
            if (exists_test(fo_name))
                return;

            std::cout << "Precomputing indexes" << std::endl;
            size_t batch_size = 1000000;
            FILE *fout = fopen(fo_name, "wb");

            std::ifstream input(path_data, ios::binary);

            float *batch = new float[batch_size * d];
            idx_t *precomputed_idx = new idx_t[batch_size];
            for (int i = 0; i < n / batch_size; i++) {
                std::cout << "Batch number: " << i+1 << " of " << n / batch_size << std::endl;

                readXvec(input, batch, d, batch_size);
                assign(batch_size, batch, precomputed_idx);

                fwrite((idx_t *) &batch_size, sizeof(idx_t), 1, fout);
                fwrite(precomputed_idx, sizeof(idx_t), batch_size, fout);
            }
            delete precomputed_idx;
            delete batch;

            input.close();
            fclose(fout);
        }


        void write(const char *path_index, const char *path_pq,
                   const char *path_norm_pq)
        {
            FILE *fout = fopen(path_index, "wb");

            fwrite(&d, sizeof(size_t), 1, fout);
            fwrite(&csize, sizeof(size_t), 1, fout);
            fwrite(&nprobe, sizeof(size_t), 1, fout);
            fwrite(&max_codes, sizeof(size_t), 1, fout);

            size_t size;
            for (size_t i = 0; i < csize; i++) {
                size = ids[i].size();
                fwrite(&size, sizeof(size_t), 1, fout);
                fwrite(ids[i].data(), sizeof(idx_t), size, fout);
            }

            for(int i = 0; i < csize; i++) {
                size = codes[i].size();
                fwrite(&size, sizeof(size_t), 1, fout);
                fwrite(codes[i].data(), sizeof(uint8_t), size, fout);
            }

            write_pq(path_pq, pq);
            write_pq(path_norm_pq, norm_pq);
            fclose(fout);
        }

        void read(const char *path_index, const char *path_pq,
                  const char *path_norm_pq)
        {
            FILE *fin = fopen(path_index, "rb");

            fread(&d, sizeof(size_t), 1, fin);
            fread(&csize, sizeof(size_t), 1, fin);
            fread(&nprobe, sizeof(size_t), 1, fin);
            fread(&max_codes, sizeof(size_t), 1, fin);

            ids = std::vector<std::vector<idx_t>>(csize);
            codes = std::vector<std::vector<uint8_t>>(csize);

            size_t size;
            for (size_t i = 0; i < csize; i++) {
                fread(&size, sizeof(size_t), 1, fin);
                ids[i].resize(size);
                fread(ids[i].data(), sizeof(idx_t), size, fin);
            }

            for(size_t i = 0; i < csize; i++){
                fread(&size, sizeof(size_t), 1, fin);
                codes[i].resize(size);
                fread(codes[i].data(), sizeof(uint8_t), size, fin);
            }

            read_pq (path_pq, pq);
            read_pq (path_norm_pq, norm_pq);
            fclose(fin);
        }

	private:
		void compute_residual(const float *x, float *residual, idx_t key)
		{
			float *centroid = (float *) quantizer->getDataByInternalId(key);
			for (int i = 0; i < d; i++){
				residual[i] = x[i] - centroid[i];
			}
		}


        void compute_average_distance(const char *path_data) const
        {
            double average = 0.0;
            size_t batch_size = 1000000;
            std::ifstream base_input(path_data, ios::binary);
            std::ifstream idx_input("/home/dbaranchuk/precomputed_idxs_999973.ivecs", ios::binary);
            std::vector<float> batch(batch_size * d);
            std::vector<idx_t> idx_batch(batch_size);

            for (int b = 0; b < 1000; b++) {
                readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
                readXvec<float>(base_input, batch.data(), d, batch_size);

                printf("%.1f %c \n", (100.*b)/1000, '%');

                for (int i = 0; i < batch_size; i++) {
                    float *centroid = (float *) quantizer->getDataByInternalId(idx_batch[i]);
                    average += faiss::fvec_L2sqr (batch.data() + i*d, centroid, d);
                }
            }
            idx_input.close();
            base_input.close();

            std::cout << "Average distance " << average / 1000000000 << std::endl;
        }
	};

}
