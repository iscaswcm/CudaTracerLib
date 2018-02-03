//This is an example renderer using the library.
//All that is done here is using the Mitsuba loader to load a scene and render a number of passes with a specified integrator.

#include <StdAfx.h>
#include <boost/filesystem.hpp>
#include <cctype>
#include <boost/optional.hpp>
#include <boost/progress.hpp>
#include <algorithm>
#include <Engine/Core.h>
#include <Engine/DynamicScene.h>
#include <SceneTypes/Node.h>
#include <Engine/Material.h>
#include <Base/Buffer.h>
#include <SceneTypes/Light.h>
#include <Engine/Image.h>
#include <Integrators/PrimTracer.h>
#include <Integrators/PathTracer.h>
#include <Integrators/PhotonTracer.h>
#include <Integrators/Bidirectional/BDPT.h>
#include <Integrators/ProgressivePhotonMapping/PPPMTracer.h>
#include <Integrators/PseudoRealtime/WavefrontPathTracer.h>
#include <Kernel/ImagePipeline/ImagePipeline.h>
#include <Engine/SceneLoader/Mitsuba/MitsubaLoader.h>
//for multiple GPU ray tracing
#include <mpi.h>
using namespace CudaTracerLib;

class SimpleFileManager : public IFileManager
{
    std::string data_path;
public:
    SimpleFileManager(const std::string& p)
        :data_path(p)
    {

    }
    virtual std::string getCompiledMeshPath(const std::string& name)
    {
        return data_path + "Compiled/" + name;
    }
    virtual std::string getTexturePath(const std::string& name)
    {
        return data_path + "textures/" + name;
    }
    virtual std::string getCompiledTexturePath(const std::string& name)
    {
        return data_path + "Compiled/" + name;
    }
};

//quick and dirty argument parsing since boost program options is a compiled library
struct options
{
    std::string data_path;
    std::string scene_file;
    int n_passes;
    TracerBase* tracer;
};
boost::optional<options> parse_arguments(int ac, char** av)
{
    options opt;

    auto is_number = [](const std::string& s)
    {
        return !s.empty() && std::find_if(s.begin(),
            s.end(), [](char c) { return !std::isdigit(c); }) == s.end();
    };

    std::vector<std::string> tracers = { "direct", "PT", "PT_Wave", "BDPT", "PPPM" };

    auto print_error = [&](const std::string& arg)
    {
        //print error message

        std::cout << "accepts 4 arguments : data path, scene file path, number of passes and tracer type {";
        for (auto& t : tracers)
            std::cout << t << ", ";
        std::cout << "}" << std::endl << arg << " could not be used, exiting now" << std::endl;
    };

    int n_args_used = 0;
    for (int i = 1; i < ac; i++)
    {
        std::string arg = av[i];
        if (boost::filesystem::is_directory(boost::filesystem::path(arg)))
            opt.data_path = arg + "/";
        else if (boost::filesystem::is_regular_file(boost::filesystem::path(arg)))
            opt.scene_file = arg;
        else if (is_number(arg))
            opt.n_passes = std::stoi(arg);
        else if (std::find(tracers.begin(), tracers.end(), arg) != tracers.end())
        {
            if (arg == "direct")
                opt.tracer = new PrimTracer();
            else if (arg == "PT")
                opt.tracer = new PathTracer();
            else if (arg == "PT_Wave")
                opt.tracer = new WavefrontPathTracer();
            else if (arg == "BDPT")
                opt.tracer = new BDPT();
            else if (arg == "PPPM")
                opt.tracer = new PPPMTracer();
        }
        else
        {
            print_error(arg);

            return boost::none;
        }

        n_args_used++;
    }

    if (n_args_used != 4)
    {
        print_error("");
        return boost::none;
    }

    return opt;
}

int main(int ac, char** av)
{
	int size, rank;
	MPI_Init(&ac, &av);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	auto opt_options = parse_arguments(ac, av);
    if (!opt_options)
        return 1;
    auto options = opt_options.get();

    int width = 1024, height = 1024;
    const float fov = 90;
    InitializeCuda4Tracer(options.data_path);
    SimpleFileManager fManager = { options.data_path };
    Sensor camera = CreateAggregate<Sensor>(PerspectiveSensor(width, height, fov));
    DynamicScene scene(&camera, SceneInitData::CreateForScene(1000, 30000, 100), &fManager);

    boost::optional<Vec2i> img_size;
    ParseMitsubaScene(scene, options.scene_file, std::map<std::string, std::string>(), img_size, false, false, false);
    if (img_size)
    {
        width = img_size.get().x;
        height = img_size.get().y;
    }

    Image outImage(width, height);

    options.tracer->Resize(width, height);
    options.tracer->InitializeScene(&scene);
    scene.UpdateScene();

    boost::progress_display show_progress(options.n_passes);
	//each GPU gtes roughly equal iterations, i.e. passes.
	int pass_per_GPU[size];
	int divisor   = options.n_passes / size;
	int remainder = options.n_passes % size;
	for(int i = 0; i < size; i++)
		pass_per_GPU[i] = divisor;
	// if options.n_passes is not integer times of MPI size, let the first
	// remainder MPI processes get the remainder samples. We have a
	// hypothesis that each pass has almost equal compute time. 
	if(remainder) {
		for(int i = 0; i < remainder; i++)
			pass_per_GPU[i] += 1;
	}
    for (int i = 0; i < pass_per_GPU[rank]; i++)
    {
        options.tracer->DoPass(&outImage, !i);
        ++show_progress;
    }

    //MPI process rank 0 gather other MPI process's rendering results.
    applyImagePipeline(*options.tracer, outImage, CreateAggregate<Filter>(BoxFilter(0.5f, 0.5f)));
    outImage.WriteDisplayImage("result.png");

    outImage.Free();
    DeInitializeCuda4Tracer();

	MPI_Finalize();
    return 0;
}
