project(
    'splatastic',
    'c','cpp',
    default_options: [
        'c_std=c11',
        'cpp_std=c++17' ])

# gcc_link_args = ['-static']
#add_project_link_arguments(gcc_link_args, language: ['c', 'cpp'])

py = import('python').find_installation(pure: false)
subdir('splatastic')
