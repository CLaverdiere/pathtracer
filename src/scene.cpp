#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <time.h>
#include <vector>

#include "image.hpp"
#include "model.hpp"
#include "scene.hpp"

Scene::Scene(std::string model_path, std::string model_name)
{
    std::string err;

    std::cout << "Loading model " << model_path << model_name << std::endl;
    unsigned int flags =
        tinyobj::triangulation | tinyobj::calculate_normals;

    std::string obj_filename = model_path + model_name;
    bool success = tinyobj::LoadObj(m_shapes, m_mats, err,
            obj_filename.c_str(), model_path.c_str(), flags);

    if (!success) {
        std::cout << err << std::endl;
        std::cerr << "Model failed to load, Exiting." << std::endl;
        std::exit(EXIT_FAILURE);
    } else {
        std::cout << "Model loaded successfully." << std::endl;
    }

    std::cout << "Constructing triangles" << std::endl;
    std::vector<Triangle> tris;
    for (int s=0; s < m_shapes.size(); s++) {
        Shape shape = m_shapes[s];
        tinyobj::mesh_t mesh = shape.mesh;
        for (int i=0; i < mesh.indices.size(); i += 3) {
            unsigned int j1 = mesh.indices[i]*3;
            unsigned int j2 = mesh.indices[i+1]*3;
            unsigned int j3 = mesh.indices[i+2]*3;
            vec3f v1 = to_vec3f(&mesh.positions[j1]);
            vec3f v2 = to_vec3f(&mesh.positions[j2]);
            vec3f v3 = to_vec3f(&mesh.positions[j3]);
            vec3f norm = to_vec3f(&mesh.normals[j1]);

            Triangle tri = {
                .verts[0] = v1,
                .verts[1] = v2,
                .verts[2] = v3,
                .norm = norm,
                .shape_data = &(m_shapes[s]),
                .index = i
            };

            tris.push_back(tri);
        }
    }

    std::cout << "Constructing KdTree" << std::endl;
    m_tree = new KdTree(tris);
}

vec3f Scene::shade(Ray ray, int bounce, int max_bounces)
{
    TriangleHit hit_data = m_tree->hit(ray);
    Triangle* tri = hit_data.tri;
    float dist = hit_data.dist;

    if (tri == NULL) {
        return vec3f(0.0, 0.0, 0.0);
    }

    tinyobj::mesh_t mesh = tri->shape_data->mesh;
    Material mat = m_mats[mesh.material_ids[tri->index / 3]];

    // Return black if we've bounced around enough.
    if (bounce > max_bounces) {
        return vec3f(0.0, 0.0, 0.0);
    }

    // Material properties
    Ray reflect_ray;
    reflect_ray.pos = ray.pos + dist * ray.dir;
    vec3f emittance = to_vec3f(mat.emission);
    vec3f reflectance = to_vec3f(mat.diffuse);
    /* vec3f specular = to_vec3f(mat.specular); */
    vec3f &norm = tri->norm;

    // Reflect in a random direction on the normal's unit hemisphere.
    reflect_ray.dir = rand_hemisphere_vec(norm);

    // Calculate BRDF
    float cos_theta = norm.dot(-ray.dir);
    vec3f brdf = 2 * reflectance * cos_theta;
    vec3f reflected_amt = shade(reflect_ray, bounce + 1, max_bounces);

    // For specular, reflect perfectly.
    /* Ray spec_reflect_ray; */
    /* vec3f spec_reflected_amt; */
    /* if (specular.norm() != 0.0) { */
    /*     spec_reflect_ray.pos = reflect_ray.pos; */
    /*     spec_reflect_ray.dir = ray.dir + (2 * cos_theta * norm); */
    /*     spec_reflected_amt = shade(spec_reflect_ray, bounce + 1, */
    /*             max_bounces); */
    /* } */

    // Final color
    return emittance + brdf.cwiseProduct(reflected_amt);
    /* return emittance + brdf.cwiseProduct(reflected_amt + spec_reflected_amt); */
}

void Scene::render(RenderOpts &opts, std::string outfile_path)
{
    // Seed for places we need random vector directions.
    srand(time(NULL));

    uint8_t pixels[opts.image_height * opts.image_width * 3];

    // For timing how long the rendering takes.
    auto start = std::chrono::steady_clock::now();

    // OPT: Some grid cells will finish faster than others. Consider another
    // method.
    std::vector<std::thread> threads;

    int lenx = opts.image_width / opts.x_threads;
    int leny = opts.image_height / opts.y_threads;
    for (int x=0; x < opts.x_threads; x++) {
        for (int y=0; y < opts.y_threads; y++) {
            threads.push_back(
                    std::thread(
                        &Scene::render_block, this,
                        std::ref(opts), &pixels[0], x * lenx, y * leny, lenx, leny
                        )
                    );
        }
    }

    for (int i=0; i < threads.size(); i++){
        threads[i].join();
    }

    std::cout << "Saving image to " << outfile_path << std::endl;
    write_png(outfile_path.c_str(), pixels, opts.image_width, opts.image_height);

    auto end = std::chrono::steady_clock::now();
    std::cout << "Traced image in " <<
        (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0)
        << " seconds." << std::endl;
}

void Scene::render_block(RenderOpts &opts, uint8_t *pixels,
                int startx, int starty, int lenx, int leny)
{
    // For drawing a rendering progress bar.
    /* int bar_width = 10; */
    /* int dot_inc = opts.image_height / bar_width; */

    // Top, bottom, left, right locations of frustum plane.
    float t = tan(opts.fov / 2),
           b = -t,
           l = -t,
           r = t;

    /* std::cout << "{"; */

    for (int y=starty; y < starty + leny; y++) {
        for (int x=startx; x < startx + lenx; x++) {
            float u = l + ((r - l) * (x + 0.5) / opts.image_height);
            float v = b + ((t - b) * (y + 0.5) / opts.image_width);
            v = -v;

            vec3f dir(u, v, -1.0);
            vec3f eye(0, 1.0, 4.0);
            Ray ray = { eye, unit(dir) };

            std::vector<vec3f> samples;
            for (int i=0; i < opts.num_samples; i++) {
                samples.push_back(shade(ray, 0, opts.num_bounces));
            }

            vec3f average_sample = vec_average(samples);
            write_pixel(pixels, average_sample, x, y, opts.image_width);
        }

        // Update progress bar.
        /* if (y % dot_inc == 0) { */
        /*     std::cout << "." << std::flush; */
        /* } */
    }

    /* std::cout << "}" << std::endl; */

}