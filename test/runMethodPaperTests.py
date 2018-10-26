# Run all convergence tests for the methods paper

from runAMRConvergenceTest import runTest
import os

from colorama import Fore, Style
#Fore: BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE, RESET.
#Back: BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE, RESET.
#Style: DIM, NORMAL, BRIGHT, RESET_ALL

base_output_dir = '/home/parkinsonjl/mushy-layer/test/output/'
base_output_dir = '/network/group/aopp/oceans/AW002_PARKINSON_MUSH/Test/'


if os.path.exists(base_output_dir):
	# check empty
	#if os.listdir(base_output_dir) :
	#	print(Fore.RED + 'Warning - output directory not empty \n')
	#	print(Style.RESET_ALL)
	pass
				
else:
    os.makedirs(base_output_dir)

parDir = os.path.abspath(os.pardir)
matlabFolder = os.path.join(parDir, 'matlab', 'MethodsPaper')
matlab_command = 'cd ' + matlabFolder + '; \n \n matlab -nodisplay -nosplash -nodesktop -r'

######################################
# 1) Diffusive solidification problem
#######################################
print(Fore.GREEN + 'Setup tests for solidification without flow' + Style.RESET_ALL)
physicalProblem = 'noFlow'
AMRSetup = [{'max_level': 0, 'ref_rat': 1, 'run_types': ['uniform']}, 
{'max_level': 1, 'ref_rat': 2, 'run_types': ['amr']},
{'max_level': 2, 'ref_rat': 2, 'run_types': ['amr']}]
# While testing:
#AMRSetup = [{'max_level': 0, 'ref_rat': 1, 'run_types': ['uniform']}];

Nzs 	  = [16, 32, 64]
num_procs = [1] * len(Nzs)

# Setup up the post processing command
dataFolder = os.path.join(base_output_dir, 'NoFlow')
figureName = os.path.join(dataFolder, 'noFlow.pdf')
analysis_command = matlab_command + ' "noFlowSolution(\'' + dataFolder + '\', \'' +figureName+ '\'); exit;"' 

# Run
extra_params = {'main.debug':'true'}
runTest(dataFolder, physicalProblem, AMRSetup, Nzs, num_procs, analysis_command, extra_params)

######################################
# 2) Convection in a fixed porous medium
######################################
print('Setup tests for convection in a fixed porous medium')
physicalProblem = 'convectionDB'
AMRSetup = [{'max_level': 0, 'ref_rat': 2, 'run_types': ['uniform']}, 
{'max_level': 1, 'ref_rat': 2, 'run_types': ['variable']}]

Nzs 	  = [128]
num_procs = [4]

chi  = 0.4
base_dataFolder = os.path.join(base_output_dir, 'ConvectionDB')

Da_Ra_vals = [{'Da': 1e-6, 'RaT': [1e7, 1e8, 1e9], 'lebars': [1.08, 3.07, 12.9]},
{'Da': 1e-2, 'RaT': [1e3, 1e4, 1e5, 5e5],  'lebars': [1.01, 1.41, 3.17, 5.24]}]

#[1.01, 1.41, 3.17, 5.24];
# [1.08, 3.07, 12.9];
all_job_ids = []

analysis_command = matlab_command 


for Da_Ra in Da_Ra_vals:
	Da = Da_Ra['Da']
	NuLebars = [str(a) for a in Da_Ra['lebars']]

	for Ra in Da_Ra['RaT']:

		extra_params['parameters.rayleighTemp'] = Ra
	 
		extra_params['parameters.darcy'] = Da*pow(1-chi,2)/pow(chi,3.0)
	                
		extra_params['bc.porosityHiVal']= str(chi) + ' ' + str(chi) # 0.4 0.4
		extra_params['bc.porosityLoVal']= str(chi) + ' ' + str(chi)

		output_dir = 'chi'+str(chi)+'-Da' + str(Da) + '-Ra' +  str(extra_params['parameters.rayleighTemp'])

		#extra_params = {}
		thisDataFolder = os.path.join(base_dataFolder, output_dir)
		analysis_command = ''
		job_ids = runTest(thisDataFolder, physicalProblem, AMRSetup, Nzs, num_procs, analysis_command, extra_params)
		all_job_ids = all_job_ids + job_ids


	Ra_str_vals = [str(a) for a in Ra]
	Ra_str = '{\' ' + '\',\''.join(Ra_str_vals) + '\'}'

	analysis_command = analysis_command + ' "compileNu(\'' + base_dataFolder + '\', \'' +str(chi)+ '\', \'' +str(Da)+ '\', \'' +Ra_str+ '\', \'' +str(Nzs[-1])+ '\', , \'[' + ','.join(NuLebars)+ ']\');' 

# Now do analysis
analysis_command = analysis_command + ' exit; " '
#analysis_command = '(base_dir, chi, Da, Ra, res)'

jobName = physicalProblem + '-analysis'
s = SlurmTask(base_dataFolder, jobName, '')

s.setDependency(all_job_ids)
s.setCustomCommand(analysis_command)
s.writeSlurmFile()
s.runTask()
print(Fore.GREEN + 'Submitted analysis job \n' + Fore.RESET)

# 3) Convection in a fixed porous medium with variable porosity

# 4) Fully coupled porous hole

# 5) Fixed chill Hele-Shaw cell


