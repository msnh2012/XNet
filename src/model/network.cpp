/*
 * network.cpp
 *
 *  Created on: Sep 21, 2015
 *      Author: lyx
 */

#include "network.h"

using namespace layer;
using namespace std;

namespace model {

	Network::Network(float* _data, int _data_dim, float* _label, int _label_dim,
					 int count, int _val_size, int _batch) {
		h_data = _data;
		h_label = _label;
		size = count;
		val_size = _val_size;
		batch = _batch;
		data_dim = _data_dim;
		label_dim = _label_dim;
		train_error = 100;
		val_error = 100;
		lambda = 1;
		callCuda(cudaMalloc(&data, sizeof(float) * data_dim * batch));
		callCuda(cudaMemcpy(data, h_data, sizeof(float) * data_dim * batch,
							cudaMemcpyHostToDevice));
		callCuda(cudaMalloc(&label, sizeof(float) * label_dim * batch));
		callCuda(cudaMemcpy(label, h_label, sizeof(float) * label_dim * batch,
							cudaMemcpyHostToDevice));
	}

	Network::~Network() {
		h_data = NULL;
		h_label = NULL;
		callCuda(cudaFree(data));
		callCuda(cudaFree(label));
		for (Layer* l : layers)
			delete l;
	}

	pair<float*, float*> Network::GetData() {
		return make_pair(data, label);
	}

	void Network::Train(int iteration, float half_time, float half_rate,
						float step_decrease, bool debug) {

		// train the network multiple times

		for (int k = 0; k < iteration && lambda > 5e-3; k++) {
			if (debug)
				for (int i = layers.size() - 1; i > 0; i--) {
					if (layers[i]->param_size != 0)
						utils::printGpuMax(layers[i]->param, layers[i]->param_size);
				}

			// divide the training set to small pieces

			int offset = 0;
			std::cout << "Iteration " << k + 1 << std::endl;
			for (int b = 0; b < size / batch; b++) {

				// choose a new piece and its labels

				callCuda(cudaMemcpy(data, h_data + offset * data_dim,
									sizeof(float) * data_dim * batch, cudaMemcpyHostToDevice));
				callCuda(cudaMemcpy(label, h_label + offset * label_dim,
									sizeof(float) * label_dim * batch, cudaMemcpyHostToDevice));

				// forward propagation

				for (int i = 0; i < layers.size() - 1; i++)
					layers[i]->forward();

				// back propagation

				for (int i = layers.size() - 1; i > 0; i--) {
					layers[i]->backward();
					layers[i]->update(); // update the parameters
				}
				offset += batch;

			}

			for (int i = layers.size() - 1; i > 0; i--)
				layers[i]->adjust_learning(step_decrease);

			// training error

			if (size > 0) {
				float* predict = new float[size];
				offset = 0;
				for (int b = 0; b < size / batch; b++) {
					callCuda(cudaMemcpy(data, h_data + offset * data_dim,
										sizeof(float) * data_dim * batch, cudaMemcpyHostToDevice));
					for (int i = 0; i < layers.size(); i++)
						layers[i]->forward(false);
					callCuda(cudaMemcpy(predict + offset * label_dim,
										layers[layers.size() - 1]->data,
										sizeof(float) * label_dim * batch, cudaMemcpyDeviceToHost));
					offset += batch;
				}
				int errors = 0;
				for (int i = 0; i < size; i++)
					if (abs(h_label[i] - predict[i]) > 0.1)
						errors++;

				train_error = errors * 100.0 / size;
				std::cout << "Train error: " << train_error << std::endl;
				delete[] predict;
			}

			// validation error

			if (val_size > 0) {
				float* predict = new float[val_size];
				offset = 0;
				for (int b = 0; b < val_size / batch; b++) {
					callCuda(cudaMemcpy(data, h_data + (size  + offset) * data_dim,
										sizeof(float) * data_dim * batch, cudaMemcpyHostToDevice));
					for (int i = 0; i < layers.size(); i++)
						layers[i]->forward(false);
					callCuda(cudaMemcpy(predict + offset * label_dim,
										layers[layers.size() - 1]->data,
										sizeof(float) * label_dim * batch, cudaMemcpyDeviceToHost));
					offset += batch;
				}
				int errors = 0;
				for (int i = 0; i < val_size; i++)
					if (abs(h_label[size + i] - predict[i]) > 0.1)
						errors++;

				float prev_error = val_error;
				val_error = errors * 100.0 / val_size;
				std::cout << "Validation error: " << val_error << std::endl;

				// adjust the learning rate if the validation error stabilizes

				if ((prev_error - val_error) / prev_error < half_time) {
					lambda *= half_rate;
					std::cout << "-- Learning rate decreased --" << std::endl;
					for (int i = layers.size() - 1; i > 0; i--)
						layers[i]->adjust_learning(half_rate);
				}

				delete[] predict;
			}
		}

	}

	void Network::PushInput(int c, int h, int w) {
		Input* input = new Input(batch, c, h, w, data);
		layers.push_back(input);
	}

	void Network::PushOutput(int label_dim) {
		Output* output = new Output(layers.back(), label, label_dim, batch);
		layers.push_back(output);
	}

	void Network::PushConvolution(int c, int kernel, float alpha, float sigma,
								  float momentum, float weight_decay) {
		Convolution* conv = new Convolution(layers.back(), batch, c, kernel, alpha,
											sigma, momentum, weight_decay);
		layers.push_back(conv);
	}

	void Network::PushPooling(int size, int stride) {
		Pooling* pool = new Pooling(layers.back(), size, stride);
		layers.push_back(pool);
	}

	void Network::PushActivation(cudnnActivationMode_t mode) {
		Activation* activation = new Activation(layers.back(), mode);
		layers.push_back(activation);
	}

	void Network::PushReLU(int output_size, float dropout_rate, float alpha,
						   float sigma, float momentum, float weight_decay) {
		ReLU* relu = new ReLU(layers.back(), output_size, dropout_rate, alpha,
							  sigma, momentum, weight_decay);
		layers.push_back(relu);
	}

	void Network::PushSoftmax(int output_size, float dropout_rate, float alpha,
							  float sigma, float momentum, float weight_decay) {
		Softmax* softmax = new Softmax(layers.back(), output_size, dropout_rate, alpha,
									   sigma, momentum, weight_decay);
		layers.push_back(softmax);
	}

	void Network::Pop() {
		Layer* tmp = layers.back();
		layers.pop_back();
		delete tmp;
		layers.back()->next = NULL;
	}

	void Network::SwitchData(float* h_data, float* h_label, int count) {
		// switch data without modifying the batch size
		size = count;
		this->h_data = h_data;
		this->h_label = h_label;
	}

	void Network::Test(float* label) {
		int offset = 0;
		for (int b = 0; b < size / batch; b++) {
			callCuda(cudaMemcpy(data, h_data + offset * data_dim,
								sizeof(float) * data_dim * batch, cudaMemcpyHostToDevice));
			for (int i = 0; i < layers.size(); i++)
				layers[i]->forward(false);
			callCuda(cudaMemcpy(label + offset * label_dim,
								layers[layers.size() - 1]->data,
								sizeof(float) * label_dim * batch, cudaMemcpyDeviceToHost));
			offset += batch;
		}
	}

	void Network::PrintGeneral() {
		std::cout << "Neural Network" << std::endl;
		std::cout << "Layers: " << layers.size() << std::endl;
		int i = 0;
		for (Layer* l : layers)
			std::cout << " - " << i++ << ' ' << l->data_size << ' ' << l->param_size << std::endl;
	}

	void Network::PrintData(int offset, int r, int c, int precision) {
		utils::printGpuMatrix(data + offset, r * c, r, c, precision);
	}

	void Network::ReadParams(std::string dir) {
		for (int i = 1; i < layers.size() - 1; i++) {
			if (layers[i]->param_size > 0)
				utils::readGPUMatrix(dir + std::to_string(i), layers[i]->param, layers[i]->param_size);
			if (layers[i]->param_bias_size > 0)
				utils::readGPUMatrix(dir + std::to_string(i) + "_bias",
									 layers[i]->param_bias, layers[i]->param_bias_size);
		}
	}

	void Network::SaveParams(std::string dir) {
		for (int i = 1; i < layers.size() - 1; i++) {
			if (layers[i]->param_size > 0)
				utils::writeGPUMatrix(dir + std::to_string(i), layers[i]->param,
									  layers[i]->param_size);
			if (layers[i]->param_bias_size > 0)
				utils::writeGPUMatrix(dir + std::to_string(i) + "_bias",
									  layers[i]->param_bias, layers[i]->param_bias_size);
		}
		std::cout << "Params saved." << std::endl;
	}

} /* namespace model */
