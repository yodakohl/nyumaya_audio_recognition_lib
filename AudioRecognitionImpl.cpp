#include "AudioRecognitionImpl.h"
#include <iostream>
#include "feature_extract.h"

using namespace tflite;



#define TFLITE_MINIMAL_CHECK(x)                              \
  if (!(x)) {                                                \
    fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
    exit(1);                                                 \
}

double get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }

AudioRecognitionImpl::AudioRecognitionImpl(const std::string& modelPath){


	// Load model
	model = tflite::FlatBufferModel::BuildFromFile(modelPath.c_str());
	TFLITE_MINIMAL_CHECK(model != nullptr);

	InterpreterBuilder builder(*model.get(), resolver);
	builder(&interpreter);

	if(!interpreter){
		std::cout << "Error creating Interpreter" << std::endl;
	}

	const std::vector<int> inputs = interpreter->inputs();

	switch (interpreter->tensor(inputs[0])->type) {

		case kTfLiteUInt8:
			quantized = true;
			break;
		default:
			std::cerr << "Model type not compatible. Only 8bit quantized models are compatible" << std::endl;
	}

	// Allocate tensor buffers.
	TFLITE_MINIMAL_CHECK(interpreter->AllocateTensors() == kTfLiteOk);

	int t_size = interpreter->tensors_size();

	const std::vector<int> outputs = interpreter->outputs();
	TfLiteIntArray* output_dims = interpreter->tensor(outputs[0])->dims;
	output_size = output_dims->data[output_dims->size - 1];

	for(int i = 0 ; i < output_size ; i++){
		last_frames.push_back( new std::list<float>);
	}
}


AudioRecognitionImpl::~AudioRecognitionImpl(){

}

void AudioRecognitionImpl::PrintDebug()
{

	if(quantized){
		std::cout << "Inference Type is UInt8" << std::endl;
	} else {
		std::cout << "Inference Type is Float32" << std::endl;
	}
	
	std::cout << "tensors size: " << interpreter->tensors_size() << "\n";
	std::cout << "nodes size: " << interpreter->nodes_size() << "\n";
	std::cout << "inputs: " << interpreter->inputs().size() << "\n";
	std::cout << "input(0) name: " << interpreter->GetInputName(0) << "\n";

	int t_size = interpreter->tensors_size();
	for (int i = 0; i < t_size; i++) {
		if (interpreter->tensor(i)->name)
			std::cout << i << ": " << interpreter->tensor(i)->name << ", "
			<< interpreter->tensor(i)->bytes << ", "
			<< interpreter->tensor(i)->type << ", "
			<< interpreter->tensor(i)->params.scale << ", "
			<< interpreter->tensor(i)->params.zero_point << "\n";
	}

	const std::vector<int> inputs = interpreter->inputs();
	const std::vector<int> outputs = interpreter->outputs();


	std::cout << "number of inputs: " << inputs.size() << "\n";
	std::cout << "number of outputs: " << outputs.size() << "\n";
}



void PrintProfilingInfo(const profiling::ProfileEvent* e, uint32_t op_index,
                        TfLiteRegistration registration) {

	std::cout << (e->end_timestamp_us - e->begin_timestamp_us) / 1000.0 << ", Node " << op_index
            << ", OpCode " << registration.builtin_code << ", "
			<< EnumNameBuiltinOperator(static_cast<BuiltinOperator>(registration.builtin_code))<< "\n";
	std::cout << "PrintProfilingInfo" << std::endl;
}

void AudioRecognitionImpl::ProfileRun(){

	profiling::Profiler* profiler = new profiling::Profiler();
	interpreter->SetProfiler(profiler);

	profiler->StartProfiling();

	struct timeval start_time, stop_time;
	gettimeofday(&start_time, nullptr);
	for (int i = 0; i < 1000; i++) {
		if (interpreter->Invoke() != kTfLiteOk) {
			std::cout << "Failed to invoke tflite!\n";
		}
	}
	gettimeofday(&stop_time, nullptr);
	std::cout << "invoked \n";
 	std::cout << "average time: "<< (get_us(stop_time) - get_us(start_time)) / (1000 * 1000) << " ms \n";

	profiler->StopProfiling();
	auto profile_events = profiler->GetProfileEvents();
	for (int i = 0; i < profile_events.size(); i++) {
		auto op_index = profile_events[i]->event_metadata;
		const auto node_and_registration = interpreter->node_and_registration(op_index);
		const TfLiteRegistration registration = node_and_registration->second;
		PrintProfilingInfo(profile_events[i], op_index, registration);
	}
}



void AudioRecognitionImpl::SetThreadCount(size_t val)
{
	number_of_threads = val;
	if (number_of_threads != -1) {
		interpreter->SetNumThreads(number_of_threads);
	}
}


int AudioRecognitionImpl::RunDetection(const uint8_t* const data,const int mel_length)
{
	_interpret(data,mel_length);
	return smooth();
}

uint8_t* AudioRecognitionImpl::RunRawDetection(const uint8_t* const data,const int mel_length)
{
	_interpret(data,mel_length);
	
	if(quantized){
		return interpreter->typed_output_tensor<uint8_t>(0);
	}
	
	return 0;
}


void AudioRecognitionImpl::_interpret(const uint8_t* const data,const int mel_length)
{
	size_t fs = sizeof(uint8_t);
	uint8_t tmp[melcount*melframes];
	
	//Copy old content to tmp, the oldest data of size mel_length is discarded
	memcpy(tmp, melwindow+mel_length, (melcount*melframes-mel_length)*fs);
	
	//Copy new mel content to tmp
	memcpy(tmp + (melcount*melframes-mel_length),data,mel_length*fs);
	
	//Copy tmp to melwindow
	memcpy(melwindow,tmp,melcount*melframes*fs);
	
	
	if(quantized){
		auto input = interpreter->typed_input_tensor<uint8_t>(0);

		for(size_t i=0 ; i < melcount*melframes; i++)
		{
			input[i] = melwindow[i];
		}
	}

	// Run inference 
	if (interpreter->Invoke() != kTfLiteOk) {
		std::cerr << "Failed to invoke tflite!" << std::endl;
	}

}


int AudioRecognitionImpl::smooth()
{
	if(quantized){
		auto output = interpreter->typed_output_tensor<uint8_t>(0);
		return smooth_detection(output,output_size);
	} 
}



// Accumulate scores. Scores are decayed over time. A event is usally 1 second maximum 
// so after one second of no activity score should be close to zero. 
// Maximum one prediction every n frames.
int AudioRecognitionImpl::smooth_detection(uint8_t*scores,int size)
{
	if (cooldown > 0){
		cooldown --;
	}

	//Moving window, max 5 elements
	for (auto& frame : last_frames)
	{
		if (frame->size() >= 3){
			frame->pop_back();
		}
	}



	//exclude index 0 which is _unknown_
	for(int i = 1 ; i < size; ++i){
		if(scores[i] >= 250 + 5*sensitivity ){
			last_frames[i]->push_front(scores[i]);
		} else {
			last_frames[i]->push_front(0);
		}
	}


	float biggest_score = 0;
	int biggest_score_key = 0;
	
	int key = 0;
	for (auto& frame : last_frames)
	{
		//Check if the biggest score in last frames is over the threshold
		float score = std::accumulate(std::begin(*frame), std::end(*frame), 0.0);
		if(score > biggest_score){
			biggest_score = score;
			biggest_score_key = key;
		}
		key ++;
	}
	

	if(biggest_score >  (200+555*sensitivity) && cooldown == 0){
		cooldown = detection_cooldown;
		last_frames[biggest_score_key]->clear();
		return biggest_score_key;
	}

	return 0;
}


size_t AudioRecognitionImpl::get_input_data_size()
{
	int blocksize = 20;
	int frame_shift = 160; 
	int bitsize = 1;
	return blocksize*frame_shift*bitsize;
}

void AudioRecognitionImpl::SetSensitivity(float sens)
{
	if( sens <= 1.0 && sens > 0){
		this->sensitivity = 1.0 - sens;
	}
}



