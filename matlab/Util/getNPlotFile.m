% General function to get either first/last/any other plot file
% Set frame = 0 to get first plot file in folder
% Set frame = -1 to get last plot file in folder
% Use any other to get the specific frame

function plotFile = getNPlotFile(output_dir, plot_prefix, frame)
if nargin < 2
    plot_prefix = '';
    
    
end

if nargin < 3
    frame = -1;
end

if output_dir(end) ~= '/'
    output_dir = [output_dir, '/'];
end

dim = 2;
subcycled = true;
frames = [];

%plot_prefix = 'AMRConvergenceTest';

search = [output_dir, plot_prefix, '*'];
files = dir(search);

actual_plot_prefix = '';
for file_i = 1:length(files)
   % fprintf('   > filename: %s \n', files(file_i).name);
   full_fname = files(file_i).name;
    
    if length(strfind(full_fname, '.2d.hdf5')) > 0 && ...
            length(strfind(full_fname, 'chk')) == 0
        
        
        fname = strrep(full_fname, '.2d.hdf5', '');
        %fprintf('   > fname: %s \n', fname);
        
        % Get final number - frame
        matchStr = regexp(fname,'\d+$','match');
        
        frame = str2num(matchStr{1});
        
        frames(end+1) = frame;
        actual_plot_prefix = strrep(fname, matchStr{1}, '');
%       
%             parts = strsplit(fname, '-');
% 
%             if length(parts) > 1
%                 frames(end+1) = str2num(parts{end});
%                 actual_plot_prefix = strrep(fname, parts{end}, '');
%             end
%         
%        
    
    end
    
end

% Default - assume the user has specified the frame they actually want
desiredFrame = frame;

if frame == 0
    % Get the first frame
    desiredFrame = min(frames);
elseif frame == -1
    % Get the last frame
    desiredFrame = max(frames);  
end

%frame = max(frames);
%frame = 0; % testing

fprintf('    > determined plot prefix = %s and frame = %d \n', actual_plot_prefix, desiredFrame);

if length(desiredFrame) > 0 && length(actual_plot_prefix) > 0
    plotFile = MushyLayerOutput(dim, desiredFrame, output_dir, actual_plot_prefix, subcycled);
else
    plotFile = []; 
end

end