import json

def save_falcon_grid_to_file(filename="scene.json", count=100000, spacing=5.0):
    entities = [
        {
            "name": "SceneRoot",
            "parent": None,
            "transform": {
                "pos": [0.0, 0.0, 0.0],
                "rot": [0.0, 0.0, 0.0, 1.0],
                "scale": [1.0, 1.0, 1.0]
            }
        },
        {
            "name": "Sun",
            "parent": 0,
            "transform": {
                "pos": [-0.57735, -0.57735, -0.57735],
                "rot": [0.0, 0.0, 0.0, 1.0],
                "scale": [1.0, 1.0, 1.0]
            },
            "directional_light": {
                "enabled": True,
                "color": [1.0, 1.0, 1.0],
                "intensity": 1.0,
                "direction_local": [-0.57735, -0.57735, -0.57735]
            }
        }
    ]

    # Grid dimensions (10x10x10 = 1000)
    dim = 10

    for i in range(count):
        # Calculate grid positions
        x = (i % dim) * spacing
        y = ((i // dim) % dim) * spacing
        z = (i // (dim * dim)) * spacing

        falcon = {
            "name": f"Falcon {i + 1}",
            "parent": 0,
            "transform": {
                "pos": [float(x), float(y), float(z) - 15.0],
                "rot": [0.0, 0.0, 0.0, 1.0],
                "scale": [0.2, 0.2, 0.2]
            },
            "mesh": {
                "path": "assets/models/falcon.vkb",
                "pipeline_domain": "world"
            }
        }
        entities.append(falcon)

    scene_data = {
        "version": 2,
        "entities": entities
    }

    # Write the dictionary to a JSON file
    try:
        with open(filename, "w") as f:
            json.dump(scene_data, f, indent=2)
        print(f"Successfully generated {filename} with {count} entities.")
    except Exception as e:
        print(f"An error occurred while writing the file: {e}")

if __name__ == "__main__":
    save_falcon_grid_to_file()
