# Script to test convergence of uniform and AMR solutions
import os
import sys

parDir = os.path.abspath(os.pardir)
pythonDir = os.path.join(parDir, 'python')
sys.path.append(pythonDir)

from classes.MushyLayerRun import MushyLayerRun
import math
from subprocess import Popen
from util.mushyLayerRunUtils import constructRunName, readInputs, writeParamsFile, isPowerOfTwo
from AMRConvergenceTest import AMRConvergenceTest

import getopt


def runTest(data_dir, physicalProblem, AMRSetup, Nzs, num_procs, analysis_command = '', numRestarts=0):

    mushyLayerBaseDir = os.path.abspath(os.pardir)

    all_params = []

    for setup in AMRSetup:
        max_level = setup['max_level']
        ref_rat = setup['ref_rat']
    
        finestRefinement = pow(ref_rat, max_level)
        maxRefinement = ref_rat**max_level

        Nz_i = -1

        # Construct the params files 
        for Nz_coarse in Nzs:

            Nz_i = Nz_i + 1

            #num_proc = pow(Nz_coarse/32, 2)
            #num_proc = max(num_proc, 1)
            #print('Nz coarse = %d, initial num proc = %d \n' % (Nz_coarse, num_proc))
            #num_proc = min(num_proc, 16)
            #print('Final num_proc = %d' % num_proc)
           
            
            # Some default options
            runTypes = ['uniform']
            gridfile = ''
            gridFile = mushyLayerBaseDir + '/grids/leftRight/' + str(Nz_coarse) + 'x' + str(Nz_coarse)

            defaultParamsFile = os.path.join(mushyLayerBaseDir, '/params/convergenceTest/'+physicalProblem+'.parameters')
            if os.path.exists(defaultParamsFile):
                params = readInputs(defaultParamsFile)

            output_dir = 'AMRConvergenceTest/' + physicalProblem + '/'
            
            if physicalProblem == 'noFlow':
                output_dir = 'AMRConvergenceTestNoFlow'
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/noFlowConvTest.parameters')
                
                
                Nx_coarse = 4;
                params['main.domain_width'] = str(4.0*float(Nx_coarse)/float(Nz_coarse))
                linearTgradient = 1.0/4.0 # delta T / height
                params['main.refine_thresh'] = str(linearTgradient*16.0/float(Nz_coarse))
                params['main.tag_buffer_size'] = str(int(float(Nz_coarse)/8))
                params['main.steady_state'] = '1e-8' #str(1e-4 * pow(32.0/float(Nz_coarse),2))
                params['main.max_grid_size'] = '32'
            elif physicalProblem == 'convectionDB':
                #output_dir = 'AMRConvergenceTestConvectionDB'
                Nx_coarse = Nz_coarse
                #numRestarts = 1
                gridFile = mushyLayerBaseDir + '/grids/leftRight/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/convectionDarcyBrinkmanConvTest.parameters')
                
                runTypes = ['uniform', 'variable']
                #runTypes = ['uniform']
                theseNzs = [32,64,128]

                if Nz_coarse not in theseNzs:
                    continue
                
                params['main.refine_thresh'] = str(3.0/float(Nz_coarse))
                params['main.tag_buffer_size']=str(max(2,int(float(Nz_coarse)/8)))
                
                params['parameters.rayleighTemp'] = '1e9'
                Da = 1e-6
               
                chi = 0.4
                
                params['parameters.darcy'] = Da*pow(1-chi,2)/pow(chi,3.0)
                
                params['bc.porosityHiVal']= str(chi) + ' ' + str(chi) # 0.4 0.4
                params['bc.porosityLoVal']= str(chi) + ' ' + str(chi)
                
                params['main.steady_state'] = '1e-8'
                params['main.max_time'] = '50000000.0'
                cfl = '0.0001'
                cfl = '0.005'            
                params['main.cfl'] = cfl
                params['main.initial_cfl'] = cfl
                params['main.max_dt'] = '1e-1'
                
                #params['main.max_step'] = '2'
                
                            
                #params['main.darcyTimescale'] = 'true'
                
                output_dir = 'AMRConvergenceTest/ConvectionDB/chi'+str(chi)+'-Da' + str(Da) + '-Ra' +  params['parameters.rayleighTemp']  + '-cfl0.001' 
                
                params['main.plot_interval'] = '1000' #str(int(100.0*float(Nz_coarse)/16.0))
                params['main.checkpoint_interval'] = params['main.plot_interval']
                
              
                params['main.time_integration_order'] = '2'
                    
     
            elif physicalProblem == 'DBVariablePorosity':
                integrationTime = '1.6'
                
                Nx_coarse = Nz_coarse;
                
                if ref_rat == 2:
                    gridFile = mushyLayerBaseDir + '/grids/middleXSmall/' + str(Nx_coarse) + 'x' + str(Nz_coarse)           
            
                else:
                    gridFile = mushyLayerBaseDir + '/grids/middleXSmall/' + str(Nx_coarse*2) + 'x' + str(Nz_coarse*2)   
                            
                    
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/DBVariablePorosityConvTest.parameters')
                
                runTypes = ['uniform', 'amr', 'variable']
                #runTypes = ['uniform', 'amr']
                #runTypes = ['uniform']
                #runTypes = ['amr']
                
                # For a fixed dt:
                dt = 0.01*float(128.0/float(Nz_coarse))
                numSteps = float(integrationTime)/dt
                
                params['main.plot_interval'] = str(int(numSteps/10.0))
                
                params['main.max_step'] = '100000'
                
                params['main.min_time'] = integrationTime
                params['main.max_time'] = integrationTime
                # Velocity refine threshold is highly dependent on the integration time
                
                #params['main.vel_refine_thresh'] = '0.0015'
                #params['main.vel_refine_thresh'] = '0.05'
                
                #params['main.refine_thresh'] = str(0.07*float(32.0/float(Nz_coarse)))
                #params['main.refine_thresh'] = str(0.07*float(32.0/float(Nz_coarse)))
                params['main.vel_refine_thresh'] = '0.0001'
                params['main.stdev'] = '0.002'
                
                regrid_int = 4 #int(4.0*float(Nx_coarse)/16.0)
            
                

                print('Determined max refinement: ' + str(maxRefinement))

                #maxGridSize = max(Nx_coarse*maxRefinement/4,4)

                # Make sure we always have one grid
                maxGridSize = max(Nx_coarse*maxRefinement,4)

                print('Max grid size: ' + str(maxGridSize))

                #bf = max(maxGridSize/2,4)
                bf = max(Nx_coarse*maxRefinement/8,4)


                gridBuffer = 0 #max(bf/4,1)
                tagBuffer = 0 #gridBuffer

                if max_level == 2:
                    gridBuffer = max(bf/4, 4)

                #if ref_rat == 4:
                #    bf = bf/2
                #   gridBuffer = bf
                #   tagBuffer = gridBuffer


                params['main.block_factor'] = str(bf)
                params['main.grid_buffer_size'] = str(gridBuffer)
                params['main.tag_buffer_size'] = str(tagBuffer)
                params['main.regrid_interval']= str(regrid_int) + ' ' + str(regrid_int) + ' ' + str(regrid_int)
                params['main.max_grid_size'] = str(maxGridSize) # Must be greater than block factor

                # To pretend AMR is VM:
                #params['regrid.tagCenterOnly'] = '0.375'
                #params['main.tag_buffer_size'] = '0'
                #params['main.grid_buffer_size'] = '0'

                params['projection.eta'] = '0.96'

                params['parameters.darcy'] = '1e-4'
                params['parameters.prandtl'] = '1.0'
                params['parameters.rayleighTemp'] = '1e7'
                params['main.periodic_bc'] = '0 1 1'
                
                chi = '0.05'
                params['bc.porosityHiVal'] = chi + ' ' + chi # 0.4 0.4
                params['bc.porosityLoVal'] = chi + ' ' + chi
                
               

                params['main.fixed_dt'] = str(dt)
                params['main.plot_interval'] = '50'
                
                params['main.checkpoint_interval'] = params['main.plot_interval']
                
                # 1 - linear
                # 2 - gaussian
                params['parameters.porosityFunction'] = '2'
                params['main.time_integration_order'] = '2'
                
                if params['parameters.porosityFunction'] == '1':
                    output_dir = 'AMRConvergenceTest/DBVariablePorosityLinear'
                elif params['parameters.porosityFunction'] == '2':
                    output_dir = 'AMRConvergenceTest/DBVariablePorosityGaussian'
                else:
                    output_dir = 'AMRConvergenceTest/DBVariablePorosity'
                    
                output_dir = output_dir + str(num_proc) + 'proc' + '-t' + integrationTime + '-v2/'
                    
                
            
            elif physicalProblem == 'DirectionalDB':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/DBDirectionalConvTest.parameters')
                output_dir = 'AMRConvergenceTestDirectionalDB'
                Nx_coarse = int(float(Nz_coarse)/2.0)
                
                pltInt = int(200.0*float(Nz_coarse)/16.0)
                pltInt = min(1000, pltInt)
                
                params['main.plot_interval'] = str(pltInt)
                params['main.checkpoint_interval'] = str(pltInt)
                
                params['main.refine_thresh'] = str(3.0/float(Nz_coarse))
                #params['main.tag_buffer_size'] = '2' # str(int(float(Nz_coarse)/8))
                #params['main.plot_interval'] = '1'
                
            elif physicalProblem == 'DirectionalDarcy':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/DarcyDirectionalConvTest.parameters')
                output_dir = 'AMRConvergenceTestDirectionalDarcy'
                Nx_coarse = int(float(Nz_coarse)/2.0)
                
                pltInt = str(int(200.0*float(Nz_coarse)/16.0))
                
                #pltInt = '2'
                params['main.plot_interval'] = pltInt
                params['main.chk_interval'] = pltInt
                
                params['main.refine_thresh'] = str(3.0/float(Nz_coarse))
                #params['main.tag_buffer_size'] = '4' # str(int(float(Nz_coarse)/8))
                #params['main.plot_interval'] = '1'
                params['projector.verbosity'] = '0'
                params['main.verbosity'] = '2' 
                params['main.grid_buffer_size'] = str(Nz_coarse/4) + " " + str(Nz_coarse/4) + " " + str(Nz_coarse/4)
                
                #params['main.regrid_interval']='16 16  4'

            elif physicalProblem == 'MushyDB':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/MushyDBConvTest.parameters')

                output_dir = 'AMRConvergenceTest/MushyDB/'
                
                Nx_coarse = Nz_coarse/2 #int(float(Nz_coarse)/2.0)
                gridFile = mushyLayerBaseDir + '/grids/topMiddle/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                
                #params['main.grid_buffer_size'] = str(Nz_coarse/4) + " " + str(Nz_coarse/4) + " " + str(Nz_coarse/4)

                #params['main.initialPerturbation'] = '0.05'
                maxTime = 32.0
                params['main.max_time'] = str(maxTime)
                #params['main.min_time'] = str(maxTime)

                dt = 0.125*32.0/float(Nz_coarse)
                #dt = (50.0/1024.0)*32.0/float(Nz_coarse)
                params['main.fixed_dt'] = str(dt)


                pltInt = int(32.0*float(Nz_coarse)/64.0)
                params['main.plot_interval'] = str(pltInt)
                params['main.chk_interval'] = params['main.plot_interval']


                max_grid = float(Nz_coarse)/2.0
                bf = float(Nz_coarse)/4.0

                max_grid = max(max_grid, 4.0)
                bf = max(bf, 2.0)

                params['main.block_factor'] = str(int(bf))
                params['main.max_grid_size']=str(int(max_grid))

                params['regrid.marginalPorosityLimit'] = '0.99'
                params['main.tag_buffer_size']=str(int(max(bf/4.0, 2.0)))


                #runTypes = ['uniform', 'amr', 'variable']
                if Nz_coarse < 512:
                    runTypes = ['uniform', 'amr']
                else:
                    runTypes = ['uniform']

            elif physicalProblem == 'MushWellResolved':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/MushWellResolvedConvTest.parameters')

                output_dir = 'AMRConvergenceTest/MushWellResolved/'
                
                Nx_coarse = Nz_coarse #int(float(Nz_coarse)/2.0)
                gridFile = mushyLayerBaseDir + '/grids/topMiddle/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                
                #params['main.grid_buffer_size'] = str(Nz_coarse/4) + " " + str(Nz_coarse/4) + " " + str(Nz_coarse/4)

                #params['main.initialPerturbation'] = '0.05'
                maxTime = 1000000.0
                params['main.max_time'] = str(maxTime)
                #params['main.min_time'] = str(maxTime)

               


                pltInt = int(200.0*float(Nz_coarse)/64.0)
                params['main.plot_interval'] = str(pltInt)
                params['main.chk_interval'] = params['main.plot_interval']

            elif physicalProblem == 'mushQuick':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/mushQuick.parameters')

                output_dir = 'AMRConvergenceTest/MushQuick/'
                
                Nx_coarse = Nz_coarse #int(float(Nz_coarse)/2.0)
                gridFile = mushyLayerBaseDir + '/grids/topMiddle/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                          
                dt = 0.025*32.0/float(Nx_coarse)
                params['main.fixed_dt'] = dt


                pltInt = int(math.ceil(10.0*float(Nz_coarse)/32.0))
                params['main.plot_interval'] = str(pltInt)
                params['main.chk_interval'] = params['main.plot_interval']


                #runTypes = ['uniform', 'amr', 'variable']
                runTypes = ['uniform']

            elif physicalProblem == 'MushyConvection':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/MushyConvection.parameters')

                output_dir = 'AMRConvergenceTest/MushyConvection/'
                
                Nx_coarse = Nz_coarse #int(float(Nz_coarse)/2.0)
                gridFile = mushyLayerBaseDir + '/grids/topMiddle/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                 
                dt64 = 0.02    
                dt64 = 5e-5    
                dt = dt64*64.0/float(Nx_coarse)
                params['main.fixed_dt'] = dt
                params['main.max_time'] = 2e-4

                pltInt = int(math.ceil(0.5*float(Nz_coarse)/32.0))
                pltInt = max(pltInt, 1)
                params['main.plot_interval'] = str(pltInt)
                params['main.chk_interval'] = params['main.plot_interval']


                #runTypes = ['uniform', 'amr', 'variable']
                runTypes = ['uniform']
                params['projection.eta'] = 0.0 # stop some issues which arise from running with the OPT code on a 64x64 grid for some reason

            elif physicalProblem == 'MushyConvectionLiquid' or physicalProblem == 'MushyConvectionLiquid2':
                

                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/'+physicalProblem+'.parameters')

                # For short sim:
                dt64 = 0.000125
                maxTime = 0.001
                
               

                # For longer sim:

                sameDt = -1
                

                if  physicalProblem == 'MushyConvectionLiquid2':
                    dt64 = 0.01
                    dt64 = 0.016

                    maxTime = 0.0001 #0.04
                    sameDt = 0.0005
                    dt64 = sameDt
                    maxTime = dt64*2*80 #sameDt*100 #93

                    maxTime = 0.5
                    #sameDt = 0.0001


                    #params['main.radius']=0.1

                    sameDt=-1

                    params['bc.bulkConcentrationLoVal']='-1 -1'

                else:
                    maxTime = 0.1
                    dt64 = 0.0005

                #dt64 = 0.001                
                #params['main.radius']=0.005
                #params['parameters.rayleighTemp']=1e8

                # Try  larger hole with smaller RaT
                #maxTime = 0.01
                #params['main.radius']=0.01
                #params['parameters.rayleighTemp']=1e7

                output_dir = 'AMRConvergenceTest/'+physicalProblem+'-t' + str(maxTime) + '/'
                #output_dir = 'AMRConvergenceTest/'+physicalProblem+'-t' + str(maxTime) + '-1proc/'
                

                Nx_coarse = Nz_coarse #int(float(Nz_coarse)/2.0)

                Nx_grid = Nx_coarse
                if ref_rat == 4:
                    Nx_grid = 2*Nx_coarse

                gridFile = mushyLayerBaseDir + '/grids/middleXSmall/' + str(Nx_grid) + 'x' + str(Nx_grid)
                 
                
                #dt64 = 0.000125
                dt = dt64*64.0/float(Nx_coarse)
                if sameDt > 0:
                    params['main.fixed_dt'] = sameDt #dt
                else:
                    params['main.fixed_dt'] = dt #dt

                params['main.max_time'] = maxTime

                pltInt = int(math.ceil(4*float(Nz_coarse)/32.0))
                pltInt = max(pltInt, 1)
                regrid_int = max(pltInt/4, 1)

                pltInt = 800 # large interval, save disk space
                params['main.plot_interval'] = str(pltInt)
                params['main.checkpoint_interval'] = params['main.plot_interval']

                # For AMR
                #params['main.vel_refine_thresh'] = 0.005
                #params['regrid.tagCenterOnly'] = 0.4

                if 'main.vel_refine_thresh' in params:
                    params.pop('main.vel_refine_thresh')

                params['main.taggingVar'] = 3 # porosity
                params['main.refine_thresh'] = float(params['main.radius'])*1.0/float(Nz_coarse) #0.05*64.0/float(Nz_coarse)



                bf = max(int(Nx_coarse/4), 4)
                
                maxGridSize = bf*2
                gbs = max(bf/8, 2)

                params['main.block_factor'] = str(bf)
                params['main.grid_buffer_size'] = gbs
                params['main.tag_buffer_size'] = 0
                params['main.regrid_interval']= str(regrid_int) + ' ' + str(regrid_int) + ' ' + str(regrid_int)
                params['main.max_grid_size'] = str(maxGridSize) # Must be greater than block factor


                #runTypes = ['uniform', 'amr', 'variable']
                if Nx_coarse < Nzs[-1]/finestRefinement:
                    runTypes = ['uniform', 'amr']
                else:
                    runTypes = ['uniform']

                #runTypes = ['uniform']   
                #params['projection.eta'] = 0.0 # stop some issues which arise from running with the OPT code on a 64x64 grid for some reason

                # Ensure middle of grid is liquid
                #params['bc.enthalpyLoVal'] = '0.0 6.2'

                # Trying to make things smoother/better resolved
                #params['main.radius'] = 0.02

            elif physicalProblem == 'MushyConvectionAnalyticVel':
                params = readInputs(mushyLayerBaseDir + '/params/convergenceTest/MushyConvection.parameters')

                output_dir = 'AMRConvergenceTest/MushyConvectionAnalyticVel/'
                
                Nx_coarse = Nz_coarse #int(float(Nz_coarse)/2.0)
                gridFile = mushyLayerBaseDir + '/grids/topMiddle/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
                 
                dt64 = 0.02    
                dt64 = 0.005     
                dt = dt64*64.0/float(Nx_coarse)
                params['main.fixed_dt'] = dt

                pltInt = int(math.ceil(1.0*float(Nz_coarse)/32.0))
                pltInt = max(pltInt, 1)
                params['main.plot_interval'] = str(pltInt)
                params['main.chk_interval'] = params['main.plot_interval']

                params['parameters.rayleighComp']=1e3
                params['main.analyticVel'] = 1
                params['main.max_time'] = 0.001
                params['main.fixed_dt'] = 0.0005

                #runTypes = ['uniform', 'amr', 'variable']
                runTypes = ['uniform']

            # Always turn off slope limiting for convergence tests
            params['main.use_limiting'] = 'false'
            params['main.debug'] = 'false' # also turn off debug to save disk space

               
            numCellsAMR = str(Nx_coarse) + ' ' + str(Nz_coarse) + '  8'    
            #gridFile = mushyLayerBaseDir + '/grids/topQuarter/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
            #gridFile = mushyLayerBaseDir + '/grids/rightHalf/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
            #
            #
            
            gridFileQuarter = mushyLayerBaseDir + '/grids/rightQuarter/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
            gridFileThreeLevels = mushyLayerBaseDir + '/grids/rightHalfThreeLevel/' + str(Nx_coarse) + 'x' + str(Nz_coarse)
            numCellsUniform = str(Nx_coarse*finestRefinement) + ' ' + str(Nz_coarse*finestRefinement) + '  8'
            numCellsUniform = str(Nx_coarse) + ' ' + str(Nz_coarse) + '  8'

            #params['main.gridfile'] = gridFile
            params['main.num_cells'] = numCellsAMR
            params['main.max_level'] = str(max_level)
            params['main.ref_ratio']= str(ref_rat) + ' ' + str(ref_rat) + ' ' + str(ref_rat) 
            
            num_proc = num_procs[Nz_i]
            params['num_proc'] = num_proc

            if num_proc > 1:
                optimalGrid = float(Nx_coarse)/(float(num_proc)/2.0)

                print('Initial optimal grid guess: %d' % optimalGrid)
                
                # increase to next power of 2
                while not isPowerOfTwo(optimalGrid):
                    optimalGrid = optimalGrid + 1

                print('Final optimal grid guess: %d' % optimalGrid)
                
                maxGrid = max(16, optimalGrid)
                # grid size must be greater than the blocking factor 
                maxGrid = max(maxGrid, 2*int(params['main.block_factor'])) 
                params['main.max_grid_size'] = str(int(maxGrid))
            

            # Now construct vector different param sets
            param_sets = []
            
            # Run 1: uniform mesh
            if 'uniform' in runTypes:
                p0 = dict(params)
                p0['main.max_level'] = '0'
                p0['main.num_cells'] = numCellsUniform
                p0['run_name'] = 'Uniform'
                p0['concise_run_name'] = 'Uniform'
            
                param_sets.append(p0)
            
            # This should do the job for AMR simulations
            if 'amr' in runTypes:
                p1 = dict(params)
                p1['main.reflux_scalar'] = '1'
                p1['main.use_subcycling'] = '1'
                p1['main.refluxType'] = '2'
                p1['projection.eta'] = '0.95'
                
                p1['run_name'] = 'AMR-Subcycle-Reflux-Freestream'+str(p1['projection.eta'])+'-MaxLevel' + str(max_level) 
                p1['concise_run_name'] = 'AMR'
                #p13['main.initialize_VD_corr'] = 'true' # true by default
            
                param_sets.append(p1)
            
            
            # Variable mesh
            if 'variable' in runTypes and max_level==1:    
                p2 = dict(params)
                p2['main.reflux_scalar'] = '1'
                p2['main.use_subcycling'] = '1'
                p2['main.refluxType'] = '2'
                p2['projection.eta'] = '0.95'

                p2['main.gridfile'] = gridFile
                p2['run_name'] = 'VariableMesh2SubcycleRefluxFreestream'+str(p1['projection.eta']) 
                p2['concise_run_name'] = 'VM'
            
                param_sets.append(p2)
            
            if 'variable' in runTypes and max_level==2: 
                p3 = dict(params)
                p3['main.reflux_scalar'] = '1'
                p3['main.use_subcycling'] = '1'
                p3['main.refluxType'] = '2'
                p3['projection.eta'] = '0.95'

                p3['main.gridfile'] = gridFileThreeLevels
                p3['run_name'] = 'VM3LevelsSubcycleRefluxFreestream0.45' +str(p3['projection.eta']) 
                p3['concise_run_name'] = 'VM'
                
                param_sets.append(p3)
            
            
            
            # Do this for all the param sets just made:
            for p in param_sets:
                
                
                #if num_proc > 1:
                #    p['run_name'] = 'p' + p['run_name']
                
                run_name = p['run_name']
                run_name = run_name
                if int(p['main.max_level']) > 0:
                     run_name = run_name + '-ref' + str(ref_rat)
                     
                run_name = run_name  + '-' + physicalProblem + '-' + str(Nz_coarse)
                
                p['concise_run_name'] = p['concise_run_name'] + str(Nz_coarse) + physicalProblem
                p['main.plot_prefix'] = run_name + '-'
                p['main.chk_prefix'] =  'chk' + p['main.plot_prefix']
                
                # Now add to the full list of param sets
                all_params.append(p)
            

        # Actually run the convergence test
        full_output_dir = os.path.join(data_dir, output_dir)
        #print('Data dir: ' + data_dir + '\n')
        #print('Output dir: ' + output_dir + '\n')
        print('Full output dir: ' + full_output_dir + '\n')
        
        
        AMRConvergenceTest(all_params, full_output_dir, physicalProblem, Nzs, num_procs, numRestarts, analysis_command)


def main(argv):
    
    # Defaults:
    num_proc = 4
    
    try:
       opts, args = getopt.getopt(argv,"n:f:H:")
    except getopt.GetoptError:
       print 'runAMRConvergenceTest.py -n <num processors>'
       sys.exit(2)
    for opt, arg in opts:
        if opt in ("-n"):
            num_proc = int(arg)
        
    print 'Num processors: ', str(num_proc)

    #################################
    # These shouldn't change
    #################################
    os.environ["CH_TIMER"] = "1"

    data_dir = os.getcwd() # for local runs
    data_dir = '/network/group/aopp/oceans/AW002_PARKINSON_MUSH/' # for writing to shared storage

    # Change things below here
    #num_proc = 1
    Nzs = [16, 32,64,128,256,512]
    #Nzs = [128]

    #num_procs = num_proc+0*Nzs
    num_procs = [1,1,2,2,4,4,8,8,8,8,8,8]
    #num_procs = [1,1,4,4,4,4,4,4,4]
    #num_procs = [1,1,1,1,1,1,1,1,1,1,1]

    #num_procs = [16]

    #Nzs = [512,256,128,64,32] # more efficient to do it this way around
    
    # By default, don't try and restart from existing files
    #numRestarts = 0


    #max_level = 0
    #ref_rat = 2

    AMRSetup = [{'max_level': 1, 'ref_rat': 2},
    {'max_level': 1, 'ref_rat': 4},
    {'max_level': 2, 'ref_rat': 2}]

    #AMRSetup = [{'max_level': 1, 'ref_rat': 2}]

    #AMRSetup = [{'max_level': 0, 'ref_rat': 2}]

    #mushQuick, MushyDB, DBVariablePorosity, DirectionalDarcy, convectionDarcyBrinkman, DirectionalDB
    # MushyConvectionAnalyticVel, MushyConvection, MushyConvectionLiquid
    physicalProblem = 'MushyConvectionLiquid2' 

    runTest(data_dir, physicalProblem, AMRSetup, Nzs, num_procs)

            

if __name__ == "__main__":
    print('Running script')
    main(sys.argv[1:])
                 

